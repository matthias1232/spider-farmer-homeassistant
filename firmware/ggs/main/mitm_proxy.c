#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "cJSON.h"

#include "sb_config.h"
#include "mqtt_wire_parser.h"
#include "ha_mqtt.h"
#include "provisioning.h"
#include "live_log.h"
#include "device_cache.h"
#include "device_registry.h"
#include "sf_command_handler.h"
#include "sf_normalizer.h"
#include "plan_text.h"
#include "plan_store.h"
#include "sf_alarm.h"
#include "wan_gate.h"
#include "config_poll.h"
#include "mitm_proxy.h"

static const char *TAG = "mitm_proxy";

// Embedded certificates, see main/CMakeLists.txt EMBED_TXTFILES.
extern const uint8_t sf_ca_pem_start[]        asm("_binary_sf_ca_pem_start");
extern const uint8_t sf_ca_pem_end[]          asm("_binary_sf_ca_pem_end");
extern const uint8_t proxy_cert_pem_start[]   asm("_binary_proxy_cert_pem_start");
extern const uint8_t proxy_cert_pem_end[]     asm("_binary_proxy_cert_pem_end");
extern const uint8_t proxy_key_pem_start[]    asm("_binary_proxy_key_pem_start");
extern const uint8_t proxy_key_pem_end[]      asm("_binary_proxy_key_pem_end");

// ---------------------------------------------------------------------------
// Single active controller session. The rebuilt firmware intentionally
// supports only one at a time — the previous version's multi-session
// registry was part of what made the earlier codebase hard to debug.
// ---------------------------------------------------------------------------
typedef struct {
    bool active;
    volatile bool closing;      // tearing down: no new commands accepted
    bool cloud_connected;
    bool cloud_reconnecting;    // a reconnect_upstream_task is in flight
    char mac[16];               // e.g. "7C2C67F03DAC", learned from the UP topic
    char slug[24];              // topic identifier for this controller; the
                                // user can rename it, so it is re-read from
                                // the registry on every status frame
    char uid[64];               // from the controller's own frames

    // Liveness, tracked from the MQTT keepalive rather than from data.
    //
    // A controller with nothing to report still sends PINGREQ on a fixed
    // interval, so silence here means the link is genuinely gone, while
    // silence in the data stream only means nothing changed.
    uint32_t last_heard_ms;     // any bytes from the controller
    uint32_t last_packet_ms;    // last complete packet (cadence estimate)
    uint32_t last_ping_ms;      // most recent PINGREQ
    uint32_t keepalive_ms;      // learned interval, 0 until two pings seen
    bool     presumed_gone;     // marked offline by the liveness check
    char     end_reason[32];    // why the relay loop exited, for the log
    uint32_t last_conn_pub_s;   // rate limit for the connection topics
    char down_topic_prefix[8];  // "CB" by default, learned from the UP topic
    mbedtls_ssl_context client_ssl;    // connection: controller -> bridge
    mbedtls_net_context client_fd;
    mbedtls_ssl_context upstream_ssl; // connection: bridge -> real SF cloud
    mbedtls_net_context upstream_fd;
    SemaphoreHandle_t lock;
    // Commands from other tasks (web, MQTT, poll) for the controller. Only
    // the relay task touches the TLS contexts: it reads them, and it alone
    // writes these queued commands -- a read on one task and a write on
    // another to the same mbedTLS context is not safe, lock or no lock.
    QueueHandle_t inject_q;
    TaskHandle_t  relay_task;
} proxy_session_t;

// One queued command: the framed MQTT packet and the controller it is for
// (a slot can be reused by another controller after a reconnect).
typedef struct {
    uint8_t *buf;
    size_t   len;
    char     mac[16];
} inject_item_t;
#define INJECT_Q_LEN 8

// One slot per controller that can be connected at the same time.
//
// Each active session holds two mbedTLS contexts and its relay buffers,
// roughly 64 KB of internal RAM, so the practical limit is low and a
// connection beyond it is refused rather than accepted and starved.
static proxy_session_t s_sessions[SB_MAX_SESSIONS] = {0};

// Returns the session currently serving a given MAC, or NULL.
static proxy_session_t *session_for_mac(const char *mac)
{
    if (!mac || !mac[0]) return NULL;
    for (int i = 0; i < SB_MAX_SESSIONS; i++) {
        if (s_sessions[i].active && !s_sessions[i].closing &&
            strcasecmp(s_sessions[i].mac, mac) == 0) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

// Topic prefix for the Home Assistant side, read once from the stored
// configuration.
static char s_device_id[32] = SB_DEVICE_ID_DEFAULT;

// Sensor readings and outlet states from the most recent status frame.
//
// These are not module blocks, so they live in the device cache's
// separate live area, keyed by MAC like everything else — several
// controllers report at once and their readings must not overwrite each
// other. Kept as a trimmed copy rather than the whole frame: a full
// getDevSta is several kilobytes and most of it is already cached.
// Naive substring search over a non-terminated buffer.
//
// The payload is not NUL-terminated, so strstr() cannot be used on it.
static bool memmem_simple(const uint8_t *hay, size_t hay_len,
                          const char *needle, size_t needle_len)
{
    if (needle_len > hay_len) return false;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return true;
    }
    return false;
}

// Republishes the Home Assistant state for whichever modules appear in
// a block of configuration, so an app change shows up at once instead of
// at the next status frame.
//
// The cached block is duplicated under the lock and published after it is
// released. Holding device_cache_lock across an MQTT publish is what the
// rest of this file does in a few places, and it puts a network call
// inside a mutex that the web server's Control page also waits on.
static void publish_module_state(const char *mac, const char *slug,
                                 const char *module)
{
    if (!mac || !mac[0] || !slug || !slug[0] || !module) return;

    bool is_fan = (strcmp(module, "fan") == 0 || strcmp(module, "blower") == 0);
    bool is_light = (strcmp(module, "light") == 0 || strcmp(module, "light2") == 0);
    if (!is_fan && !is_light) return;

    device_cache_lock();
    cJSON *cached = device_cache_get(mac, module);
    cJSON *copy = cached ? cJSON_Duplicate(cached, true) : NULL;
    device_cache_unlock();

    if (!copy) return;

    if (is_fan) {
        sf_publish_fan_state(slug, module, copy);
        sf_publish_fan_speed(slug, module, copy);
        sf_publish_fan_extras(slug, module, copy);
    } else {
        // Same pointer for block and cache: this IS the merged cache
        // entry, so the modeType fallback has nowhere better to look.
        sf_publish_light_state(slug, module, copy, copy);
        sf_publish_light_extras(slug, module, copy);
    }
    cJSON_Delete(copy);
}

// Watches commands travelling from the cloud to the controller.
//
// Read-only: the packet has already been relayed byte for byte by the
// time this runs. The point is to notice settings the vendor app
// changes, since the controller does not report them back and the
// bridge would otherwise display its own stale copy.
// Just the plan's running flag, without touching (or parsing) the stages.
static void publish_plan_enabled(const char *slug, int on)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/plan/enabled", slug);
    ha_mqtt_publish_state(topic, on ? "ON" : "OFF");
}

static void observe_cloud_command(proxy_session_t *s, const mqtt_packet_t *pkt)
{
    if (!s->mac[0] || pkt->packet_type != MQTT_PUBLISH) return;
    if (!pkt->message || pkt->message_len == 0) return;

    // Only two commands matter here; skip everything else cheaply,
    // because this runs on every packet the cloud sends.
    bool is_config = memmem_simple(pkt->message, pkt->message_len,
                                   "setConfigField", 14);
    bool is_tz = memmem_simple(pkt->message, pkt->message_len,
                               "setDevTimezone", 14);
    bool is_heat = memmem_simple(pkt->message, pkt->message_len,
                                 "setSensorHeating", 16);
    if (!is_config && !is_tz && !is_heat) return;

    // A grow plan from the app: handled from the text, never parsed whole
    // (five stages are ~20 KB as a cJSON tree). Whole plan (keyPath
    // ["plan"], params.plan) or start/stop (["plan","enabled"]).
    if (is_config && memmem_simple(pkt->message, pkt->message_len, "\"plan\"", 6)) {
        const char *pl;
        size_t pll;
        if (jtext_plan_in_frame((const char *)pkt->message, pkt->message_len, &pl, &pll)) {
            // A whole plan written from the app replaces whatever the
            // bridge was driving. With a long plan active the web page
            // showed the bridge's stored stages instead of the app's new
            // ones, and the next window move would have overwritten the
            // app's plan on the controller. The long plan is kept on the
            // bridge and can be activated again from the page.
            plan_store_info_t li;
            if (plan_store_info(s->mac, &li) && li.active) {
                plan_store_set_active(s->mac, false);
                ESP_LOGI(TAG, "Long plan for %s deactivated: the app wrote a new plan", s->mac);
            }
            device_cache_set_plan_text(s->mac, pl, pll);
            sf_publish_plan(s->mac, s->slug);
            ESP_LOGI(TAG, "Grow plan changed from the app for %s (%d stage(s))",
                     s->mac, device_cache_plan_count(s->mac));
            return;
        }
        if (memmem_simple(pkt->message, pkt->message_len, "\"enabled\"", 9) &&
            pkt->message_len < 512) {
            cJSON *r = cJSON_ParseWithLength((const char *)pkt->message, pkt->message_len);
            cJSON *p = r ? cJSON_GetObjectItem(r, "params") : NULL;
            cJSON *en = p ? cJSON_GetObjectItem(p, "enabled") : NULL;
            cJSON *kp = p ? cJSON_GetObjectItem(p, "keyPath") : NULL;
            cJSON *k0 = cJSON_IsArray(kp) ? cJSON_GetArrayItem(kp, 0) : NULL;
            if (cJSON_IsNumber(en) && cJSON_IsString(k0) && strcmp(k0->valuestring, "plan") == 0) {
                device_cache_set_plan_enabled(s->mac, en->valueint);
                publish_plan_enabled(s->slug, en->valueint);
                ESP_LOGI(TAG, "Grow plan %s from the app for %s",
                         en->valueint ? "started" : "stopped", s->mac);
                cJSON_Delete(r);
                return;
            }
            cJSON_Delete(r);
        }
    }

    cJSON *root = cJSON_ParseWithLength((const char *)pkt->message,
                                        pkt->message_len);
    if (!root) return;

    cJSON *params = cJSON_GetObjectItem(root, "params");

    // Sensor cleaning started or stopped from the vendor app. Reflected at
    // once -- in the web interface (next poll) and in Home Assistant (now)
    // -- so the app's command visibly arrived, before the controller's next
    // status frame confirms phase and remaining time.
    if (is_heat && cJSON_IsObject(params)) {
        cJSON *on = cJSON_GetObjectItem(params, "on");
        if (cJSON_IsNumber(on)) {
            bool start = on->valuedouble != 0;
            int phase = 0;
            bool reported = device_cache_sensor_cleaning_phase(s->mac, &phase, NULL);
            if (start) {
                device_cache_set_sensor_cleaning_phase(s->mac, 1, 7200);
            } else if (reported && phase == 1) {
                device_cache_set_sensor_cleaning_phase(s->mac, 2, 300);
            } else {
                device_cache_set_sensor_cleaning(s->mac, false);
            }
            sf_publish_sensor_cleaning(s->mac, s->slug);
            ESP_LOGI(TAG, "Sensor cleaning %s from the app for %s",
                     start ? "started" : "stopped", s->mac);
        }
    }

    if (is_config) {
        // Alarm settings changed in the app: the command carries the
        // whole block, so it becomes the cached state at once.
        cJSON *alm = params ? cJSON_GetObjectItem(params, "alarm") : NULL;
        if (cJSON_IsObject(alm)) {
            device_cache_replace(s->mac, "alarm", alm);
            sf_publish_alarm(s->mac, s->slug);
            ESP_LOGI(TAG, "Alarm settings changed from the app for %s", s->mac);
        }

        cJSON *cal = params ? cJSON_GetObjectItem(params, "calibration") : NULL;
        if (cJSON_IsObject(cal)) {
            device_cache_merge(s->mac, "calibration", cal);

            const char *fields[] = { "temp", "humi", "co2", "ppfd" };
            for (int i = 0; i < 4; i++) {
                cJSON *v = cJSON_GetObjectItem(cal, fields[i]);
                if (cJSON_IsNumber(v)) {
                    device_registry_set_cal(s->mac, fields[i],
                                            (float)v->valuedouble);
                }
            }
            ESP_LOGI(TAG, "Calibration changed from the app for %s", s->mac);
        }

        // Everything else the app configures: fan, blower and the lights.
        //
        // This is the ONLY source these blocks have. The controller's
        // reply to setConfigField is a bare {"code":200,"msg":"ok"} with
        // no params, so the status path has nothing to merge, and the
        // config poll is held back while the cloud leg is up (see the
        // guard in the UP path). Without this the cache never learns
        // modeType, natural, closeCO2, shakeLevel, minSpeed, maxSpeed or
        // cycleTime at all — Home Assistant showed no mode and no
        // schedule, and every command built from the empty cache fell
        // back to synthesized defaults that reset the controller to
        // Manual.
        //
        // Nothing is sent to do this: the app's own command already
        // carries every field, and it has been relayed untouched by the
        // time this runs. That matters, because asking for the same data
        // with a getConfigField burst is what the poll guard exists to
        // prevent.
        //
        // Only the block the command actually addresses is taken, read
        // from keyPathrather than by scanning params for anything that
        // happens to be named like a module. The calibration branch just
        // above proves the envelope varies: it arrives as keyPath
        // ["calibration"] with no "device" prefix, while module writes
        // use ["device","fan"]. Trusting a key name alone would cache a
        // value the app never meant as that module's state — and the
        // command path now replays the whole cached block.
        cJSON *kp = params ? cJSON_GetObjectItem(params, "keyPath") : NULL;
        int kn = cJSON_IsArray(kp) ? cJSON_GetArraySize(kp) : 0;
        if (kn > 0) {
            cJSON *first = cJSON_GetArrayItem(kp, 0);
            cJSON *last = cJSON_GetArrayItem(kp, kn - 1);
            if (cJSON_IsString(first) && strcmp(first->valuestring, "plan") == 0) {
                // Handled from the text above.
            } else if (cJSON_IsString(last) && last->valuestring[0] &&
                strcmp(last->valuestring, "calibration") != 0) {
                cJSON *blk = cJSON_GetObjectItem(params, last->valuestring);
                if (cJSON_IsObject(blk)) {
                    if (cJSON_GetObjectItem(blk, "modeType"))
                        device_cache_note_mode_command(s->mac, last->valuestring);
                    device_cache_merge(s->mac, last->valuestring, blk);
                    publish_module_state(s->mac, s->slug, last->valuestring);
                }
            }
        }
    }

    // A time zone set from the vendor app.
    //
    // Checked here as well as in the status path because this is the
    // earliest point it can be seen. The status check still catches the
    // case where the controller was changed while the bridge was off.
    //
    // The command itself is relayed untouched: undoing it is a second
    // command sent afterwards, not a modification of this one. Rewriting
    // traffic in flight would leave the vendor app showing a setting
    // that was never applied.
    if (is_tz && cJSON_IsObject(params)) {
        cJSON *tzn = cJSON_GetObjectItem(params, "timezone");
        cJSON *tzp = cJSON_GetObjectItem(params, "TZ");
        if (cJSON_IsString(tzp)) {
            ESP_LOGI(TAG, "Time zone set from the app for %s: %s (%s)",
                     s->mac, cJSON_IsString(tzn) ? tzn->valuestring : "?",
                     tzp->valuestring);
            device_registry_set_tz(s->mac,
                cJSON_IsString(tzn) ? tzn->valuestring : NULL,
                tzp->valuestring);

            // Puts it back when the bridge is managing the clock. Does
            // nothing when the values already match, so an app change
            // that agrees with the bridge is left alone.
            sf_timezone_check(s->mac,
                cJSON_IsString(tzn) ? tzn->valuestring : "",
                tzp->valuestring);
        }
    }

    cJSON_Delete(root);
}

static void capture_live_state(const char *mac, cJSON *data)
{
    cJSON *sensor = cJSON_GetObjectItem(data, "sensor");
    cJSON *outlet = cJSON_GetObjectItem(data, "outlet");
    if (cJSON_IsObject(sensor) || cJSON_IsObject(outlet)) {
        device_cache_set_live(mac, sensor, outlet);
    }
}

void mitm_proxy_fill_live_state(const char *mac, void *sensors_obj,
                                void *outlets_arr)
{
    cJSON *sensors = (cJSON *)sensors_obj;
    cJSON *outlets = (cJSON *)outlets_arr;
    if (!mac || !mac[0]) return;

    device_cache_lock();
    cJSON *s_live_sensors = device_cache_sensors(mac);
    cJSON *s_live_outlets = device_cache_outlets(mac);

    // Readable labels, matching what the normaliser publishes.
    static const struct { const char *sf; const char *label; } MAP[] = {
        { "temp",     "Temperature" },
        { "humi",     "Humidity"    },
        { "vpd",      "VPD"         },
        { "co2",      "CO2"         },
        { "ppfd",     "PPFD"        },
        { "tempSoil", "Soil temp"   },
        { "humiSoil", "Soil moist"  },
        { "ECSoil",   "Soil EC"     },
    };

    if (s_live_sensors && sensors) {
        for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
            cJSON *v = cJSON_GetObjectItem(s_live_sensors, MAP[i].sf);
            if (!cJSON_IsNumber(v)) continue;
            char buf[24];
            snprintf(buf, sizeof(buf), "%g", v->valuedouble);
            cJSON_AddStringToObject(sensors, MAP[i].label, buf);
        }
    }

    if (s_live_outlets && outlets) {
        cJSON *o = NULL;
        cJSON_ArrayForEach(o, s_live_outlets) {
            if (!o->string || o->string[0] != 'O') continue;
            cJSON *on = cJSON_GetObjectItem(o, "mOnOff");
            if (!on) on = cJSON_GetObjectItem(o, "on");
            if (!on) continue;

            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "n", atoi(o->string + 1));
            cJSON_AddBoolToObject(e, "on",
                                  cJSON_IsNumber(on) ? on->valuedouble != 0
                                                     : cJSON_IsTrue(on));
            cJSON_AddItemToArray(outlets, e);
        }
    }

    device_cache_unlock();
}

static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_ctr_drbg;
static mbedtls_x509_crt s_server_cert;
static mbedtls_pk_context s_server_key;
static mbedtls_x509_crt s_upstream_ca;
static mbedtls_ssl_config s_server_conf;
static mbedtls_ssl_config s_upstream_conf;

static void tls_globals_init(void)
{
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_ctr_drbg);
    const char *pers = "spiderbridge";
    int rc = mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func, &s_entropy,
                                    (const unsigned char *)pers, strlen(pers));
    if (rc != 0) ESP_LOGE(TAG, "ctr_drbg_seed failed: -0x%04x", -rc);

    // --- Server identity: what the controller sees -----------------------
    mbedtls_x509_crt_init(&s_server_cert);
    mbedtls_pk_init(&s_server_key);

    // PEM files are embedded via EMBED_TXTFILES, i.e. with a trailing NUL
    // byte. mbedtls_x509_crt_parse() expects that NUL counted in the
    // length for PEM input, so no -1 here.
    rc = mbedtls_x509_crt_parse(&s_server_cert, proxy_cert_pem_start,
                                 proxy_cert_pem_end - proxy_cert_pem_start);
    if (rc != 0) ESP_LOGE(TAG, "Could not parse proxy_cert.pem: -0x%04x", -rc);

    rc = mbedtls_pk_parse_key(&s_server_key, proxy_key_pem_start,
                               proxy_key_pem_end - proxy_key_pem_start, NULL, 0,
                               mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (rc != 0) ESP_LOGE(TAG, "Could not parse proxy_key.pem: -0x%04x", -rc);

    mbedtls_ssl_config_init(&s_server_conf);
    rc = mbedtls_ssl_config_defaults(&s_server_conf, MBEDTLS_SSL_IS_SERVER,
                                      MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) ESP_LOGE(TAG, "ssl_config_defaults (server) failed: -0x%04x", -rc);
    mbedtls_ssl_conf_rng(&s_server_conf, mbedtls_ctr_drbg_random, &s_ctr_drbg);
    rc = mbedtls_ssl_conf_own_cert(&s_server_conf, &s_server_cert, &s_server_key);
    if (rc != 0) ESP_LOGE(TAG, "conf_own_cert (server) failed: -0x%04x", -rc);

    // Floor at TLS 1.2, no ceiling: mbedTLS then offers the highest
    // version it was built with. The real cloud negotiates TLS 1.3, and
    // the controller expects that — with TLS 1.3 missing from the build
    // it started the handshake and closed the connection without an
    // alert, over and over.
    mbedtls_ssl_conf_min_tls_version(&s_server_conf, MBEDTLS_SSL_VERSION_TLS1_2);

    // The real controller presents no client certificate. Requiring one
    // here (CERT_REQUIRED/OPTIONAL) made every session die silently right
    // after the TLS handshake — confirmed by comparing against the
    // original Python bridge, whose PROTOCOL_TLS_SERVER context defaults
    // to CERT_NONE and never asks the client to authenticate.
    mbedtls_ssl_conf_authmode(&s_server_conf, MBEDTLS_SSL_VERIFY_NONE);

    // --- Upstream: mTLS client to the real SF cloud, private CA ----------
    mbedtls_x509_crt_init(&s_upstream_ca);
    rc = mbedtls_x509_crt_parse(&s_upstream_ca, sf_ca_pem_start,
                                 sf_ca_pem_end - sf_ca_pem_start);
    if (rc != 0) ESP_LOGE(TAG, "Could not parse sf_ca.pem: -0x%04x", -rc);

    mbedtls_ssl_config_init(&s_upstream_conf);
    rc = mbedtls_ssl_config_defaults(&s_upstream_conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) ESP_LOGE(TAG, "ssl_config_defaults (client) failed: -0x%04x", -rc);
    mbedtls_ssl_conf_rng(&s_upstream_conf, mbedtls_ctr_drbg_random, &s_ctr_drbg);
    mbedtls_ssl_conf_ca_chain(&s_upstream_conf, &s_upstream_ca, NULL);
    mbedtls_ssl_conf_authmode(&s_upstream_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    // Same certificate shown to the controller also serves as our client
    // certificate for mTLS to the real cloud — matches the original.
    rc = mbedtls_ssl_conf_own_cert(&s_upstream_conf, &s_server_cert, &s_server_key);
    if (rc != 0) ESP_LOGE(TAG, "conf_own_cert (client) failed: -0x%04x", -rc);
    mbedtls_ssl_conf_min_tls_version(&s_upstream_conf, MBEDTLS_SSL_VERSION_TLS1_2);
}

// ---------------------------------------------------------------------------
// Resolving the real cloud address, bypassing our own DNS hijack.
//
// The bridge redirects sf.mqtt.spider-farmer.com to its own AP address so
// the controller connects to the proxy. The proxy itself has to reach the
// REAL cloud — resolving the name the normal way would hand it the
// hijacked answer, and the proxy would connect to itself. The session
// would then die right after CONNECT with no error anywhere, because the
// bridge would be talking to its own listener. This was the single most
// time-consuming bug in the previous version.
//
// The lookup goes straight to a public resolver, by-passing the hijack,
// and the result is cached for the process lifetime.
// ---------------------------------------------------------------------------
static bool resolve_real_cloud(char *out, size_t out_sz)
{
    static char cached[16] = "";

    if (cached[0]) {
        strncpy(out, cached, out_sz - 1);
        out[out_sz - 1] = '\0';
        return true;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 5 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in resolver = { .sin_family = AF_INET, .sin_port = htons(53) };
    inet_pton(AF_INET, "1.1.1.1", &resolver.sin_addr);

    uint8_t q[128];
    size_t p = 0;
    q[p++] = 0x5B; q[p++] = 0xA7;   // transaction id
    q[p++] = 0x01; q[p++] = 0x00;   // standard query, recursion desired
    q[p++] = 0x00; q[p++] = 0x01;   // one question
    q[p++] = 0x00; q[p++] = 0x00;
    q[p++] = 0x00; q[p++] = 0x00;
    q[p++] = 0x00; q[p++] = 0x00;

    const char *label = SB_SF_CLOUD_HOST;
    while (*label && p < sizeof(q) - 8) {
        const char *dot = strchr(label, '.');
        size_t len = dot ? (size_t)(dot - label) : strlen(label);
        if (len > 63 || p + len + 1 >= sizeof(q) - 6) break;
        q[p++] = (uint8_t)len;
        memcpy(q + p, label, len);
        p += len;
        if (!dot) break;
        label = dot + 1;
    }
    q[p++] = 0x00;
    q[p++] = 0x00; q[p++] = 0x01;   // type A
    q[p++] = 0x00; q[p++] = 0x01;   // class IN

    if (sendto(sock, q, p, 0, (struct sockaddr *)&resolver, sizeof(resolver)) < 0) {
        close(sock);
        return false;
    }

    uint8_t resp[256];
    int n = recv(sock, resp, sizeof(resp), 0);
    close(sock);

    if (n < 16) {
        ESP_LOGE(TAG, "No DNS answer for the real cloud address");
        return false;
    }
    if (((resp[6] << 8) | resp[7]) < 1) {
        ESP_LOGE(TAG, "The resolver returned no address for %s", SB_SF_CLOUD_HOST);
        return false;
    }

    // The A record is the final four bytes of a single-answer response.
    snprintf(cached, sizeof(cached), "%u.%u.%u.%u",
             resp[n - 4], resp[n - 3], resp[n - 2], resp[n - 1]);
    ESP_LOGI(TAG, "Real cloud address: %s -> %s", SB_SF_CLOUD_HOST, cached);

    strncpy(out, cached, out_sz - 1);
    out[out_sz - 1] = '\0';
    return true;
}

static bool connect_upstream(proxy_session_t *s)
{
    mbedtls_net_init(&s->upstream_fd);
    mbedtls_ssl_init(&s->upstream_ssl);

    // Connect by address, not by name, so our own hijack cannot send the
    // proxy back to itself. The TLS hostname below stays the real name,
    // so certificate validation and SNI still work correctly.
    char cloud_ip[16];
    if (!resolve_real_cloud(cloud_ip, sizeof(cloud_ip))) {
        ESP_LOGE(TAG, "Could not resolve the real cloud address");
        return false;
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", SB_SF_CLOUD_PORT);
    int rc = mbedtls_net_connect(&s->upstream_fd, cloud_ip, port_str, MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not connect to the cloud at %s: -0x%04x", cloud_ip, -rc);
        return false;
    }
    rc = mbedtls_ssl_setup(&s->upstream_ssl, &s_upstream_conf);
    if (rc != 0) {
        ESP_LOGE(TAG, "ssl_setup (upstream) failed: -0x%04x (often too little "
                      "free heap)", -rc);
        return false;
    }
    rc = mbedtls_ssl_set_hostname(&s->upstream_ssl, SB_SF_CLOUD_HOST);
    if (rc != 0) {
        ESP_LOGE(TAG, "ssl_set_hostname failed: -0x%04x", -rc);
        return false;
    }
    mbedtls_ssl_set_bio(&s->upstream_ssl, &s->upstream_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    rc = mbedtls_ssl_handshake(&s->upstream_ssl);
    if (rc != 0) {
        char errbuf[100];
        mbedtls_strerror(rc, errbuf, sizeof(errbuf));
        ESP_LOGE(TAG, "Upstream TLS handshake failed: %s", errbuf);
        return false;
    }
    ESP_LOGI(TAG, "mTLS connection to the SF cloud (%s) is up", SB_SF_CLOUD_HOST);
    ESP_LOGI(TAG, "Free internal heap with both TLS sessions: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

// Writes a raw frame back to the controller. Takes the session lock
// because Home Assistant commands are injected from another task.
// Writes a whole buffer to a TLS context, retrying what does not fit.
//
// Both sockets are non-blocking, so mbedtls_ssl_write() has two outcomes
// that are easy to miss and were both being ignored here:
//
//   * MBEDTLS_ERR_SSL_WANT_WRITE — the socket buffer is momentarily
//     full. Nothing was sent. Retrying shortly almost always succeeds.
//   * a short return — only part of the buffer went out, and the rest
//     has to be sent from where it stopped.
//
// Dropping either case silently loses a whole MQTT packet. That is
// exactly what "commands usually work but not always" looks like from
// the outside, and a lost status frame makes the vendor app show the
// controller as offline until something prompts a fresh one — which is
// why a pull-to-refresh brought it straight back.
//
// The caller already holds the session lock, so this is only ever
// retrying against one writer.
static bool tls_write_all(mbedtls_ssl_context *ssl, const uint8_t *buf,
                          size_t len)
{
    size_t sent = 0;
    // Bounded so a genuinely dead socket cannot stall the relay loop.
    // 50 x 2 ms is comfortably longer than a transient buffer-full, and
    // short enough not to matter if the peer has really gone.
    for (int attempt = 0; attempt < 50 && sent < len; attempt++) {
        int n = mbedtls_ssl_write(ssl, buf + sent, len - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        return false;   // a real error: the session is ending anyway
    }
    return sent == len;
}

static void write_to_client(proxy_session_t *s, const uint8_t *buf, size_t len)
{
    xSemaphoreTake(s->lock, portMAX_DELAY);
    bool ok = tls_write_all(&s->client_ssl, buf, len);
    xSemaphoreGive(s->lock);
    if (!ok) {
        ESP_LOGW(TAG, "Short write to the controller (%u bytes) — "
                      "packet dropped", (unsigned)len);
    }
}

// Same locking for the cloud side.
//
// mbedtls_ssl_context is not safe for concurrent use. The relay task
// already calls mbedtls_ssl_write() on upstream_ssl from inside its own
// select loop; mitm_proxy_report_calibration() was calling it directly
// from whichever task handled the HTTP or MQTT request, with no lock at
// all. Two tasks touching the same TLS context's internal state at once
// is exactly the kind of bug that corrupts it in a way a panic handler
// cannot always catch — it can wedge the task instead, which looks like
// a flashing-tool-only hang requiring a power cycle rather than a clean
// reboot.
//
// s->lock is the same mutex write_to_client() uses for the controller
// side; one mutex per session serialises both directions, which is
// simpler and sufficient here since neither path is hot.
static void write_to_cloud(proxy_session_t *s, const uint8_t *buf, size_t len)
{
    xSemaphoreTake(s->lock, portMAX_DELAY);
    bool ok = true;
    if (s->cloud_connected) {
        ok = tls_write_all(&s->upstream_ssl, buf, len);
    }
    xSemaphoreGive(s->lock);
    if (!ok) {
        ESP_LOGW(TAG, "Short write to the cloud (%u bytes) — packet dropped",
                 (unsigned)len);
    }
}

// Parses the controller's UP topic (SF/GGS/{prefix}/API/UP/{MAC}) to learn
// its MAC and the topic prefix it uses, then forwards the raw PUBLISH
// payload to Home Assistant unchanged.
static void process_client_publish(proxy_session_t *s, mqtt_packet_t *pkt)
{
    char topic_copy[256];
    strncpy(topic_copy, pkt->topic, sizeof(topic_copy) - 1);
    topic_copy[sizeof(topic_copy) - 1] = '\0';

    char *parts[8];
    int n = 0;
    char *tok = strtok(topic_copy, "/");
    while (tok && n < 8) { parts[n++] = tok; tok = strtok(NULL, "/"); }

    if (n < 6 || strcmp(parts[0], "SF") != 0 || strcmp(parts[1], "GGS") != 0 ||
        strcmp(parts[3], "API") != 0 || strcmp(parts[4], "UP") != 0) {
        return;
    }

    if (s->mac[0] == '\0') {
        strncpy(s->mac, parts[5], sizeof(s->mac) - 1);

        // Learn the controller on first sight. A brand new one is added
        // with the MAC as its name until the user renames it, so it
        // works immediately rather than waiting to be configured.
        device_entry_t *dev = device_registry_resolve(s->mac);
        if (dev) {
            strncpy(s->slug, dev->slug, sizeof(s->slug) - 1);
            ESP_LOGI(TAG, "Controller identified: %s ('%s')", s->mac, dev->name);
            device_registry_set_online(s->mac, true);
            ha_mqtt_announce_device(s->mac);

            // Corrects a stale retained calibration value.
            //
            // The registry holding these is RAM-only and resets to zero
            // on every bridge restart, but the MQTT topics are retained
            // and so can keep showing whatever was last set before that
            // restart — observed directly: a controller reporting fresh
            // offsets passively picked up before a reboot stayed visible
            // in MQTT as those same offsets long after the registry that
            // was supposed to back them had gone back to zero. HA shows
            // exactly what is retained, so from that side it looks like
            // "HA doesn't update calibration" when the actual fault is
            // the bridge never telling it the value changed back.
            ha_mqtt_publish_device_calibration(s->mac, s->slug);

            // Hand over the bridge's zone and clock, if "Apply this to
            // every controller" is on: a full time sync on every connect,
            // so a controller that was off still catches up.
            if (prov_tz_push()) sf_sync_device_time(s->mac, s->uid);
            // A long plan on the bridge: give the controller today's window.
            sf_plan_window_on_connect(s->mac, s->uid);
        } else {
            // Registry full: fall back to the MAC so the data still
            // flows, just without a friendly name.
            strncpy(s->slug, s->mac, sizeof(s->slug) - 1);
            ESP_LOGW(TAG, "No free slot for %s — using the MAC as its name", s->mac);
        }
    }
    strncpy(s->down_topic_prefix, parts[2], sizeof(s->down_topic_prefix) - 1);

    if (!pkt->message || pkt->message_len == 0) return;

    // Raw passthrough, unchanged. Only published in raw or both mode.
    ha_mqtt_publish_raw(pkt->message, pkt->message_len);

    // A grow plan is taken from the text and never parsed whole: five
    // stages are ~5 KB of JSON and would be ~20 KB as a cJSON tree.
    {
        const char *pl;
        size_t pll;
        if (memmem_simple(pkt->message, pkt->message_len, "\"plan\"", 6) &&
            jtext_plan_in_frame((const char *)pkt->message, pkt->message_len, &pl, &pll)) {
            device_cache_set_plan_text(s->mac, pl, pll);
            sf_publish_plan(s->mac, s->slug);
            return;
        }
    }

    // The Home Assistant layer needs the frame parsed.
    if (!ha_mqtt_ha_enabled()) return;

    cJSON *root = cJSON_ParseWithLength((const char *)pkt->message, pkt->message_len);
    if (!root) return;

    cJSON *uid = cJSON_GetObjectItem(root, "uid");
    if (cJSON_IsString(uid) && uid->valuestring[0]) {
        bool first = (s->uid[0] == '\0');
        strncpy(s->uid, uid->valuestring, sizeof(s->uid) - 1);
        s->uid[sizeof(s->uid) - 1] = '\0';
        if (s->mac[0]) device_registry_note_uid(s->mac, s->uid);

        // Config polling, once per session.
        //
        // The requests are injected locally, so the cloud never sees them —
        // but it does see the controller's replies, with message IDs it
        // never issued.
        //
        // This used to be refused whenever the cloud leg was up, because a
        // test that lifted the guard was followed by a hang that needed a
        // power cycle rather than a reboot. What that burst actually caused
        // was never established, and the guard had a cost of its own: every
        // schedule, cycle and threshold entity stayed empty until the user
        // happened to change something in the Spider Farmer app.
        //
        // So it runs again, spaced wider (1.5 s between requests rather than
        // 500 ms) and with instrumentation that records the round number, the
        // heap before and after every request and the lowest value seen —
        // see config_poll.c. If the device wedges again, the log says how far
        // the round got instead of stopping mid-burst, and watchdog_task()
        // prints every task's stack high-water mark at that moment.
        //
        // Keeping the instrumentation is the point. Do not quieten it down
        // to save log space before a run has been observed to complete
        // cleanly end to end.
        if (first && s->mac[0]) {
            config_poll_set_session(s->mac, s->uid);
            config_poll_request();
        }
    }

    cJSON *method = cJSON_GetObjectItem(root, "method");
    // getSysSta belongs here too: it carries the controller's own time
    // zone, local time and firmware version. Leaving it out meant those
    // frames were relayed and logged but never parsed, so the zone the
    // controller reported was never actually read.
    bool is_status = cJSON_IsString(method) &&
                     (strcmp(method->valuestring, "getDevSta") == 0 ||
                      strcmp(method->valuestring, "getSysSta") == 0 ||
                      strcmp(method->valuestring, "getConfigField") == 0 ||
                      strcmp(method->valuestring, "setConfigField") == 0);

    if (is_status) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        // setConfigField echoes carry the block under params instead.
        if (!cJSON_IsObject(data)) {
            cJSON *params = cJSON_GetObjectItem(root, "params");
            if (cJSON_IsObject(params)) data = params;
        }
        if (cJSON_IsObject(data)) {
            // getDevSta reports the fan/blower mode only when it is not
            // Manual: {"modeType":1,"on":1,"level":79} in Schedule, plain
            // {"on":1,"level":79} in Manual. A merge keeps the old modeType
            // when the field is absent, so a switch to Manual (from the app,
            // HA or a status frame that crossed our own command) left the
            // cache stuck in the previous mode -- the speed stayed locked
            // and Home Assistant kept showing the old mode. Absent means 0.
            // Guarded (device_cache_status_mode_absent): only for a
            // controller that reports modeType at all, and not right after
            // a mode command, so a stale frame cannot reset the mode.
            if (strcmp(method->valuestring, "getDevSta") == 0) {
                static const char *const FANS[] = { "fan", "blower" };
                for (int i = 0; i < 2; i++) {
                    cJSON *fb = cJSON_GetObjectItem(data, FANS[i]);
                    if (!cJSON_IsObject(fb)) continue;
                    bool has = cJSON_GetObjectItem(fb, "modeType") != NULL;
                    if (!has && !cJSON_GetObjectItem(fb, "on") && !cJSON_GetObjectItem(fb, "level"))
                        continue;
                    int how = device_cache_status_mode(s->mac, FANS[i], has);
                    if (how == MODE_FRAME_MANUAL && !has)
                        cJSON_AddNumberToObject(fb, "modeType", 0);
                    else if (how == MODE_FRAME_IGNORE && has)
                        cJSON_DeleteItemFromObject(fb, "modeType");
                }
            }
            // Merge first, so the normaliser sees schedules and modes
            // that this particular frame may not carry.
            device_cache_merge_data(s->mac, data);
            capture_live_state(s->mac, data);

            // Re-read the slug rather than trusting the copy taken at
            // connect time: renaming a device mid-session has to move
            // its topics immediately, not after the next reconnect.
            device_registry_lock();
            device_entry_t *dev = device_registry_find(s->mac);
            if (dev) {
                strncpy(s->slug, dev->slug, sizeof(s->slug) - 1);
                s->slug[sizeof(s->slug) - 1] = '\0';
            }
            device_registry_unlock();

            sf_normalize_and_publish(s->mac, s->slug, root);

            // Connection health alongside the readings, but not on every
            // frame: the controller publishes roughly every five seconds
            // and these values change slowly.
            uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
            if (now_s - s->last_conn_pub_s >= 30) {
                s->last_conn_pub_s = now_s;
                ha_mqtt_publish_connection(s->mac, s->slug);
            }
        }
    }

    cJSON_Delete(root);
}

// First active session, used by callers that predate multi-device
// support and have no particular controller in mind.
static proxy_session_t *first_active(void)
{
    for (int i = 0; i < SB_MAX_SESSIONS; i++) {
        if (s_sessions[i].active && !s_sessions[i].closing && s_sessions[i].mac[0]) return &s_sessions[i];
    }
    return NULL;
}

const char *mitm_proxy_controller_mac(void)
{
    proxy_session_t *s = first_active();
    return s ? s->mac : "";
}

const char *mitm_proxy_controller_uid(void)
{
    proxy_session_t *s = first_active();
    return s ? s->uid : "";
}

bool mitm_proxy_cloud_reconnecting(void)
{
    for (int i = 0; i < SB_MAX_SESSIONS; i++)
        if (s_sessions[i].active && s_sessions[i].cloud_reconnecting) return true;
    return false;
}

bool mitm_proxy_has_session(void)
{
    return first_active() != NULL;
}

bool mitm_proxy_device_online(const char *mac)
{
    return session_for_mac(mac) != NULL;
}

void mitm_proxy_drop_sessions(const char *why)
{
    int dropped = 0;
    for (int i = 0; i < SB_MAX_SESSIONS; i++) {
        if (!s_sessions[i].active) continue;
        // Clearing active makes the relay loop fall out on its next
        // pass, which then runs the normal teardown — rather than
        // tearing the TLS contexts down from another task underneath a
        // loop that is still using them.
        strncpy(s_sessions[i].end_reason, why ? why : "settings changed",
                sizeof(s_sessions[i].end_reason) - 1);
        // closing, not active=false: the slot stays taken until the
        // session task has finished its teardown, so the listener cannot
        // hand it to a new connection while the old contexts are freed.
        s_sessions[i].closing = true;
        dropped++;
    }
    if (dropped) {
        ESP_LOGI(TAG, "Ended %d session(s): %s. The controller reconnects "
                      "on its own.", dropped, why ? why : "settings changed");
    }
}

const char *mitm_proxy_uid_for(const char *mac)
{
    proxy_session_t *s = session_for_mac(mac);
    return s ? s->uid : "";
}

void mitm_proxy_liveness(const char *mac, uint32_t *silence_s,
                         uint32_t *keepalive_s)
{
    if (silence_s) *silence_s = 0;
    if (keepalive_s) *keepalive_s = 0;

    proxy_session_t *s = session_for_mac(mac);
    if (!s || !s->last_heard_ms) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (silence_s) *silence_s = (now_ms - s->last_heard_ms) / 1000;
    if (keepalive_s) *keepalive_s = s->keepalive_ms / 1000;
}

// Sends a command to one specific controller. With several connected,
// the MAC decides which session it goes to; passing an empty MAC falls
// back to the first active one.
bool mitm_proxy_inject_command_to(const char *mac, const char *payload, size_t len)
{
    proxy_session_t *sess = (mac && mac[0]) ? session_for_mac(mac) : first_active();
    if (!sess) {
        ESP_LOGW(TAG, "inject: no active session for %s",
                 (mac && mac[0]) ? mac : "any controller");
        return false;
    }
    if (len == 0) return false;
    // The controller reads a frame as one TLS record of at most
    // MBEDTLS_SSL_IN_CONTENT_LEN on its side as well.
    if (len > SB_PLAN_FRAME_MAX) {
        ESP_LOGW(TAG, "Command payload too large to inject (%u bytes, max %u)",
                 (unsigned)len, (unsigned)SB_PLAN_FRAME_MAX);
        return false;
    }

    char topic[96];
    snprintf(topic, sizeof(topic), "SF/GGS/%s/API/DOWN/%s",
             sess->down_topic_prefix[0] ? sess->down_topic_prefix : "CB",
             sess->mac);

    // Heap, sized to this command: most are ~130 bytes, a grow plan up to
    // ~5 KB; a fixed buffer of the largest size wasted that on every one.
    size_t cap = len + strlen(topic) + 16;
    uint8_t *out = malloc(cap);
    if (!out) {
        ESP_LOGE(TAG, "Out of memory building the command packet");
        return false;
    }

    int out_len = mqtt_build_publish(topic, (const uint8_t *)payload, len,
                                     0, false, 1, out, cap);
    bool ok = out_len > 0;
    if (ok) {
        // Handed to the relay task, which writes it on its next pass
        // (within ~0.1 s). The relay task's own calls queue too, without
        // waiting, so it can never block on itself.
        inject_item_t it = { .buf = out, .len = (size_t)out_len };
        strncpy(it.mac, sess->mac, sizeof(it.mac) - 1);
        TickType_t wait = (xTaskGetCurrentTaskHandle() == sess->relay_task) ? 0 : pdMS_TO_TICKS(500);
        if (!sess->inject_q || xQueueSend(sess->inject_q, &it, wait) != pdTRUE) {
            ESP_LOGW(TAG, "Command queue to %s full -- command dropped", sess->mac);
            free(out);
            return false;
        }
        out = NULL;   // owned by the queue now
        ESP_LOGI(TAG, "Command injected on %s (%u bytes)", topic, (unsigned)len);
        live_log_add(LOG_DIR_HA_IN, MQTT_PUBLISH, topic,
                     (const uint8_t *)payload, len);

        // A verbatim copy of this command is deliberately NOT mirrored
        // to the cloud.
        //
        // That was tried first: forwarding the exact setConfigField
        // command onto the UP topic. It made things worse — three
        // session drops in four minutes plus a device restart, versus
        // roughly one drop every few minutes without it. The cloud
        // tracks the message ids it issued downstream; a DOWN-shaped
        // command reappearing on the UP channel is not a shape it ever
        // expects a controller to send, and it reacted by closing the
        // connection.
        //
        // What calibration actually needs is handled separately in
        // mirror_calibration_reply_to_cloud(): a synthetic reply shaped
        // like a real getConfigField response, which is a shape the
        // cloud does expect on that channel.
    } else {
        ESP_LOGW(TAG, "Command could not be framed (%u bytes)", (unsigned)len);
    }
    free(out);
    return ok;
}

void mitm_proxy_inject_command(const char *payload, size_t len)
{
    mitm_proxy_inject_command_to(NULL, payload, len);
}

bool mitm_proxy_calibration_throttled(const char *mac)
{
    // Minimum spacing between calibration commands, for one device.
    //
    // Reproduced directly: firing rapid-fire calibration changes (~20
    // within 4 seconds, as a dragged UI slider or a retry loop would
    // produce) reliably crashes the device badly enough to need a power
    // cycle, even with the write_to_cloud() lock in place protecting the
    // mbedTLS context. The lock fixes concurrent access from two tasks;
    // it does not fix whatever breaks under high call *frequency* from
    // one path, which has not been isolated further. Until it is, the
    // one proven defence is to not let that frequency occur.
    //
    // Checked once, before either the DOWN command to the controller or
    // the synthetic UP reply to the cloud is built — both are skipped
    // together for a throttled call, since both are what the stress test
    // fired in the same tight loop. The last value sent is still what
    // the controller holds; the next change past the window, or the next
    // periodic calibration poll, reports it.
    static struct { char mac[16]; int64_t last_us; } s_last[SB_MAX_SESSIONS];
    int64_t now_us = esp_timer_get_time();
    const int64_t MIN_INTERVAL_US = 1200000;  // 1.2 s

    int slot = -1;
    for (int i = 0; i < SB_MAX_SESSIONS; i++) {
        if (s_last[i].mac[0] && strcasecmp(s_last[i].mac, mac) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < SB_MAX_SESSIONS; i++) {
            if (!s_last[i].mac[0]) { slot = i; break; }
        }
    }
    if (slot < 0) return false;  // table full; fail open rather than block

    if (s_last[slot].mac[0] &&
        (now_us - s_last[slot].last_us) < MIN_INTERVAL_US) {
        return true;
    }
    strncpy(s_last[slot].mac, mac, sizeof(s_last[slot].mac) - 1);
    s_last[slot].last_us = now_us;
    return false;
}

void mitm_proxy_report_calibration(const char *mac, const char *cal_json)
{
    // Deliberately a no-op now.
    //
    // This used to synthesise an unsolicited getConfigField reply on the
    // UP channel, with a self-generated msgId that did not correlate to
    // any request the cloud had sent. Tested live, twice: the cloud
    // never reflected the change into the Spider Farmer app even after
    // an explicit manual refresh there, which means the cloud most
    // likely only trusts a getConfigField reply it can match to its own
    // outstanding request id — an unprompted push with a made-up id is
    // the kind of message a correctly-written client silently discards,
    // not something that updates its display.
    //
    // Meanwhile this code path was also responsible for two confirmed
    // hard crashes requiring a power cycle (a concurrent mbedTLS write
    // from two tasks, and a rapid-fire burst problem whose root cause
    // was never fully isolated beyond "don't call this often"). With the
    // one thing it was for confirmed not to work, that risk is no longer
    // worth carrying.
    //
    // What IS confirmed true: a calibration value set through the bridge
    // reaches the real controller and is genuinely applied there (a
    // commanded +3.1 degree offset was observed to shift the published
    // temperature reading by exactly that amount). The controller is not
    // the problem. The vendor cloud's own record of calibration appears
    // to be populated only by watching a request/reply pair it initiated
    // itself — which is exactly what the real app's own getConfigFile +
    // setConfigField flow looks like — and there is no evidence any
    // message from the bridge on the UP channel can substitute for that
    // without knowing the cloud's own request id scheme, which is not
    // something available to reverse-engineer safely against a
    // production service.
    //
    // Net effect: calibration set via the bridge/HA correctly affects
    // the controller and therefore Home Assistant; it will not be
    // reflected in the Spider Farmer app until the app's own next
    // getConfigFile exchange happens to be initiated from the app side.
    // That is a real, known limitation, not a bug being silently masked.
    (void)mac;
    (void)cal_json;
}

// Answers a controller packet locally, used when cloud forwarding is off
// and there is no upstream peer to reply on the controller's behalf.
//
// Without these replies the controller considers the session dead and
// reconnects every few seconds, never settling long enough to publish
// anything.
static void answer_locally(proxy_session_t *s, const mqtt_packet_t *pkt)
{
    uint8_t out[64];
    int len = -1;

    switch (pkt->packet_type) {
        case MQTT_CONNECT:
            len = mqtt_build_connack(out, sizeof(out));
            ESP_LOGI(TAG, "Local mode: answered CONNECT with CONNACK");
            break;

        case MQTT_PINGREQ:
            len = mqtt_build_pingresp(out, sizeof(out));
            break;

        case MQTT_PUBLISH:
            // Only QoS 1 expects an acknowledgement.
            if (pkt->qos == 1) {
                len = mqtt_build_puback(pkt->packet_id, out, sizeof(out));
            }
            break;

        case MQTT_SUBSCRIBE: {
            uint16_t pid = mqtt_packet_id_of(pkt->payload, pkt->payload_len);
            int topics = mqtt_count_subscribe_topics(pkt->payload, pkt->payload_len);
            if (topics < 1) topics = 1;
            len = mqtt_build_suback(pid, topics, 0x01, out, sizeof(out));
            ESP_LOGI(TAG, "Local mode: granted %d subscription(s)", topics);
            break;
        }

        default:
            break;
    }

    if (len > 0) {
        write_to_client(s, out, (size_t)len);
        live_log_add(LOG_DIR_DOWN, (uint8_t)(out[0] >> 4),
                     "(answered locally)", NULL, 0);
    }
}

// ---------------------------------------------------------------------------
// Relay loop: mirrors bytes in both directions and parses the controller's
// PUBLISH packets along the way so their payload can be forwarded to Home
// Assistant.
// ---------------------------------------------------------------------------
// --- Reconnecting the cloud leg without disturbing the controller ---
//
// Runs on its own task: connect_upstream() does DNS, a TCP connect and a
// full TLS 1.3 handshake, which can take a few seconds — far too long to
// block relay_loop() and the controller's own traffic while it happens.
// Only one such task may run per session at a time; cloud_reconnecting
// guards that, since a flaky cloud can close the link again shortly after
// the controller reconnects and the previous attempt is still running.
static void reconnect_upstream_task(void *arg)
{
    proxy_session_t *s = (proxy_session_t *)arg;
    // A short pause: the real cloud closing one connection and refusing
    // the next within the same second has been observed, and retrying
    // instantly just burns the handshake's heap and CPU for nothing.
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (s->active && !s->closing && wan_gate_cloud_forward()) {
        bool ok = connect_upstream(s);
        // relay_loop()'s select() only sets this once, right before its
        // own loop starts; a reconnect needs it done here instead.
        if (ok) mbedtls_net_set_nonblock(&s->upstream_fd);
        xSemaphoreTake(s->lock, portMAX_DELAY);
        if (s->active && !s->closing && !s->cloud_connected) {
            s->cloud_connected = ok;
        } else if (ok) {
            // The session ended, or somehow reconnected already, while
            // this was in flight: close what was just opened instead of
            // leaking it or clobbering a newer connection.
            mbedtls_ssl_close_notify(&s->upstream_ssl);
            mbedtls_ssl_free(&s->upstream_ssl);
            mbedtls_net_free(&s->upstream_fd);
        }
        xSemaphoreGive(s->lock);
        if (ok) ESP_LOGI(TAG, "Cloud link restored for %s", s->mac[0] ? s->mac : "(unknown)");
        else ESP_LOGW(TAG, "Cloud still unreachable — the controller's own session continues locally");
    }
    s->cloud_reconnecting = false;
    vTaskDelete(NULL);
}

static void reconnect_upstream_async(proxy_session_t *s)
{
    if (s->cloud_reconnecting) return;
    s->cloud_reconnecting = true;
    if (xTaskCreate(reconnect_upstream_task, "sb_cloud_reco", 6144, s, 4, NULL) != pdPASS) {
        s->cloud_reconnecting = false;
        ESP_LOGW(TAG, "Could not start the cloud reconnect task — retrying next time it closes");
    }
}

// Writes the queued commands to the controller (relay task only). Items
// for another controller (a slot reused after a reconnect) are dropped.
static void drain_injects(proxy_session_t *s, bool send)
{
    inject_item_t it;
    while (s->inject_q && xQueueReceive(s->inject_q, &it, 0) == pdTRUE) {
        if (send && strcasecmp(it.mac, s->mac) == 0) write_to_client(s, it.buf, it.len);
        free(it.buf);
    }
}

static void relay_loop(proxy_session_t *s)
{
    s->relay_task = xTaskGetCurrentTaskHandle();
    // All three of these live on the heap, not the stack.
    //
    // Two 1536-byte buffers plus an eight-entry mqtt_packet_t array come
    // to well over 4 KB, and mbedTLS 1.3 adds its own frames on top of
    // that inside mbedtls_ssl_read(). On the stack this overflowed the
    // session task right after the cloud connection came up — the whole
    // chain worked and then the device rebooted.
    uint8_t *client_buf = malloc(SB_TLS_RX_BUF_SIZE);
    uint8_t *tmp = malloc(SB_TLS_RX_BUF_SIZE);
    mqtt_packet_t *pkts = malloc(sizeof(mqtt_packet_t) * 8);

    if (!client_buf || !tmp || !pkts) {
        ESP_LOGE(TAG, "Out of memory setting up the relay buffers");
        free(client_buf);
        free(tmp);
        free(pkts);
        return;
    }

    size_t client_buf_len = 0;

    mbedtls_net_set_nonblock(&s->client_fd);
    if (s->cloud_connected) {
        mbedtls_net_set_nonblock(&s->upstream_fd);
    }

    while (s->active && !s->closing) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s->client_fd.fd, &rfds);
        int maxfd = s->client_fd.fd;

        if (s->cloud_connected) {
            FD_SET(s->upstream_fd.fd, &rfds);
            if (s->upstream_fd.fd > maxfd) maxfd = s->upstream_fd.fd;
        }

        // Short timeout: queued commands go out within 0.1 s.
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        if (s->inject_q && uxQueueMessagesWaiting(s->inject_q)) tv.tv_usec = 0;
        int rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) break;

        drain_injects(s, true);

        // --- Liveness check ---
        //
        // The TCP connection can stay open long after the controller has
        // gone: a device unplugged or carried out of range sends no FIN,
        // so the socket looks healthy indefinitely. Only the absence of
        // its regular traffic reveals it, and Home Assistant needs that
        // promptly rather than at the next reconnect.
        //
        // The allowance is generous relative to the observed cadence
        // (~5 s), because the controller does go briefly quiet during
        // reconnects and while the cloud session restarts. Marking a
        // working device unavailable is the worse error, so the floor
        // keeps short-lived gaps from ever tripping it.
        if (s->mac[0] && s->last_heard_ms) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t silence = now_ms - s->last_heard_ms;

            uint32_t limit = SB_LIVENESS_FALLBACK_MS;
            if (s->keepalive_ms) {
                limit = s->keepalive_ms * 8;
                if (limit < SB_LIVENESS_MIN_MS) limit = SB_LIVENESS_MIN_MS;
                if (limit > SB_LIVENESS_FALLBACK_MS) limit = SB_LIVENESS_FALLBACK_MS;
            }

            if (silence > limit && !s->presumed_gone) {
                ESP_LOGW(TAG, "%s silent for %u s (keepalive %u s) — "
                              "marking it unavailable",
                         s->mac, (unsigned)(silence / 1000),
                         (unsigned)(s->keepalive_ms / 1000));
                s->presumed_gone = true;
                device_registry_set_online_why(s->mac, false,
                                               "stopped responding");
                ha_mqtt_publish_device_availability(s->slug, false);
            } else if (silence <= limit && s->presumed_gone) {
                // It came back without the session ever dropping.
                ESP_LOGI(TAG, "%s is responding again", s->mac);
                s->presumed_gone = false;
                device_registry_set_online(s->mac, true);
                ha_mqtt_publish_device_availability(s->slug, true);
            }
        }

        if (FD_ISSET(s->client_fd.fd, &rfds)) {
            int n = mbedtls_ssl_read(&s->client_ssl, tmp, SB_TLS_RX_BUF_SIZE);
            if (n > 0) {
                // Mirror to the real cloud when forwarding is on, so the
                // vendor app keeps working.
                //
                // Locked the same way write_to_cloud() is: this is the
                // relay task's own normal path and rarely contends, but
                // a synthetic calibration reply can now also write to
                // upstream_ssl from a different task, and mbedTLS
                // contexts are not safe for concurrent use from two
                // tasks regardless of how rarely it happens.
                write_to_cloud(s, tmp, (size_t)n);

                // Any bytes from the controller mean it is alive, whether
                // or not they complete a packet the parser can use.
                s->last_heard_ms = (uint32_t)(esp_timer_get_time() / 1000);

                // Also parse it so PUBLISH payloads reach Home Assistant.
                //
                // A packet that cannot fit the buffer at all would stay at
                // its front forever: nothing after it is ever parsed again,
                // the device looks silent and is marked unavailable while
                // it keeps talking (seen after a 5-stage plan reply). Such
                // a packet is skipped -- it was relayed above unchanged.
                if (client_buf_len + n > SB_TLS_RX_BUF_SIZE) {
                    ESP_LOGW(TAG, "Parse buffer overrun (%u + %d bytes) -- resynchronising",
                             (unsigned)client_buf_len, n);
                    client_buf_len = 0;
                }
                if (client_buf_len + n <= SB_TLS_RX_BUF_SIZE) {
                    memcpy(client_buf + client_buf_len, tmp, n);
                    client_buf_len += n;
                }
                size_t consumed = 0;
                int cnt = mqtt_parse_packets(client_buf, client_buf_len, pkts, 8, &consumed);
                for (int i = 0; i < cnt; i++) {
                    // Log every packet type, not just PUBLISH, so the
                    // live view shows the whole session rather than only
                    // the data frames.
                    live_log_add(LOG_DIR_UP, pkts[i].packet_type,
                                 pkts[i].topic[0] ? pkts[i].topic : NULL,
                                 pkts[i].message, pkts[i].message_len);

                    if (pkts[i].packet_type == MQTT_PUBLISH) {
                        process_client_publish(s, &pkts[i]);
                    }

                    // --- Liveness ---
                    //
                    // Measured against the real controller: it never
                    // sends PINGREQ. Over 150 s it sent 180 PUBLISH, 30
                    // CONNECT and 30 SUBSCRIBE, and no keepalive at all.
                    // So the MQTT keepalive cannot be the liveness
                    // signal here, and anything built on it would simply
                    // never fire.
                    //
                    // What it does do is publish roughly every five
                    // seconds. That steady cadence is the better signal:
                    // it is already there, needs nothing from the
                    // controller, and stops the moment the device does.
                    //
                    // The interval is learned from consecutive packets
                    // rather than assumed, so a controller with a
                    // different rhythm still gets a sensible timeout.
                    {
                        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
                        if (s->last_packet_ms) {
                            uint32_t gap = now_ms - s->last_packet_ms;
                            // Ignore bursts and long pauses; only a
                            // plausible cadence updates the estimate.
                            if (gap > 500 && gap < 120000) {
                                s->keepalive_ms = s->keepalive_ms
                                    ? (s->keepalive_ms * 3 + gap) / 4  // smoothed
                                    : gap;
                            }
                        }
                        s->last_packet_ms = now_ms;
                        s->last_heard_ms = now_ms;
                    }

                    // With no cloud peer the bridge has to acknowledge
                    // itself, otherwise the controller drops the session.
                    if (!s->cloud_connected) {
                        answer_locally(s, &pkts[i]);
                    }
                }
                memmove(client_buf, client_buf + consumed, client_buf_len - consumed);
                client_buf_len -= consumed;
                // A buffer full of bytes that do not parse (a corrupt or
                // over-long packet) is dropped so parsing can resume.
                if (cnt == 0 && client_buf_len >= SB_TLS_RX_BUF_SIZE - 64) {
                    ESP_LOGW(TAG, "Unparseable data in the buffer -- dropped %u bytes",
                             (unsigned)client_buf_len);
                    client_buf_len = 0;
                }
            } else if (n <= 0 && n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
                ESP_LOGI(TAG, "Controller connection closed");
                strncpy(s->end_reason, "controller closed",
                        sizeof(s->end_reason) - 1);
                break;
            }
        }

        if (s->cloud_connected && FD_ISSET(s->upstream_fd.fd, &rfds)) {
            int n = mbedtls_ssl_read(&s->upstream_ssl, tmp, SB_TLS_RX_BUF_SIZE);
            if (n > 0) {
                // Cloud -> controller, passed through unchanged. Home
                // Assistant commands are written separately and never
                // travel via the cloud.
                write_to_client(s, tmp, (size_t)n);

                // Parsed for the log, and to notice settings the vendor
                // app changes. The bytes themselves are relayed
                // untouched above — nothing here alters the stream.
                mqtt_packet_t down[4];
                size_t used = 0;
                int dn = mqtt_parse_packets(tmp, (size_t)n, down, 4, &used);
                for (int i = 0; i < dn; i++) {
                    live_log_add(LOG_DIR_DOWN, down[i].packet_type,
                                 down[i].topic[0] ? down[i].topic : NULL,
                                 down[i].message, down[i].message_len);

                    // Settings changed from the vendor app pass through
                    // here. Noticing them means the interface updates at
                    // once rather than at the next poll.
                    observe_cloud_command(s, &down[i]);
                }
            } else if (n <= 0 && n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
                // The real Spider Farmer cloud closes its end of the link
                // every minute or so — observed on every session, not an
                // error condition on our side. Ending the whole session
                // here used to drop the controller's own, perfectly
                // healthy connection along with it: the app then saw the
                // device flicker offline every time, and any app command
                // that happened to arrive in that instant was lost.
                //
                // The controller side is untouched: its socket, its
                // buffered bytes and its MQTT session all keep running.
                // Only the upstream half is torn down and marked closed,
                // so the liveness/keepalive logic below continues to
                // answer the controller locally until a fresh upstream
                // connection is established.
                ESP_LOGI(TAG, "Cloud link closed — continuing the controller's "
                              "session locally and reconnecting to the cloud");
                xSemaphoreTake(s->lock, portMAX_DELAY);
                mbedtls_ssl_free(&s->upstream_ssl);
                mbedtls_net_free(&s->upstream_fd);
                s->cloud_connected = false;
                xSemaphoreGive(s->lock);
                reconnect_upstream_async(s);
            }
        }
    }

    free(client_buf);
    free(tmp);
    free(pkts);
}

static void handle_session(proxy_session_t *s)
{
    relay_loop(s);
    // From here on nothing may be queued for the contexts being freed:
    // session_for_mac() stops handing the session out once it is closing
    // (s->active stays true until the very end so the listener cannot
    // reuse the slot mid-teardown), then whatever was queued is dropped.
    s->closing = true;
    drain_injects(s, false);
    s->relay_task = NULL;
    // relay_loop() only returns once s->active is false (set below) or the
    // controller's own link ended; either way a cloud reconnect task may
    // still be mid-handshake. s->active is already false going into this
    // function in the first case, so the task's own check of it would
    // race with the frees here — wait it out instead (a failed handshake
    // can take several seconds; a connecting one rarely longer).
    for (int i = 0; i < 100 && s->cloud_reconnecting; i++) vTaskDelay(pdMS_TO_TICKS(100));

    mbedtls_ssl_close_notify(&s->client_ssl);
    mbedtls_ssl_free(&s->client_ssl);
    mbedtls_net_free(&s->client_fd);

    if (s->cloud_connected) {
        mbedtls_ssl_close_notify(&s->upstream_ssl);
        mbedtls_ssl_free(&s->upstream_ssl);
        mbedtls_net_free(&s->upstream_fd);
        s->cloud_connected = false;
    }

    // Stop the config poller from sending into a closed session.
    config_poll_set_session(NULL, NULL);

    // Availability is per controller, so only this device goes offline.
    if (s->mac[0]) {
        device_registry_set_online_why(s->mac, false,
                                       s->end_reason[0] ? s->end_reason
                                                        : "session ended");
        ha_mqtt_publish_device_availability(s->slug, false);
    }

    memset(s->mac, 0, sizeof(s->mac));
    memset(s->slug, 0, sizeof(s->slug));
    memset(s->uid, 0, sizeof(s->uid));
    memset(s->end_reason, 0, sizeof(s->end_reason));
    // Liveness belongs to one session: carried over, a stale
    // presumed_gone or timestamp made the next session look silent.
    s->last_heard_ms = 0;
    s->last_packet_ms = 0;
    s->keepalive_ms = 0;
    s->presumed_gone = false;
    drain_injects(s, false);   // anything queued during the teardown
    s->active = false;
}

static void session_task(void *arg)
{
    proxy_session_t *s = (proxy_session_t *)arg;

    // The TLS handshake runs here, on the session's own large stack, not
    // in the listener: mbedTLS 1.3 needs several KB of stack for it, and
    // keeping the listener small saves that permanently.
    int rc = mbedtls_ssl_handshake(&s->client_ssl);
    if (rc != 0 || !s->active) {
        char errbuf[100];
        mbedtls_strerror(rc, errbuf, sizeof(errbuf));
        ESP_LOGE(TAG, "TLS handshake with the controller failed: %s (-0x%04x)", errbuf, -rc);
        if (rc == MBEDTLS_ERR_SSL_ALLOC_FAILED) {
            ESP_LOGE(TAG, "  Out of memory — free internal heap: %u bytes",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        } else if (rc == MBEDTLS_ERR_SSL_CONN_EOF) {
            ESP_LOGE(TAG, "  The client closed the connection during the "
                          "handshake, likely rejecting our certificate.");
        } else if (rc == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) {
            ESP_LOGE(TAG, "  The client sent a fatal alert — often an "
                          "expired certificate or a wrong clock.");
        }
        mbedtls_ssl_free(&s->client_ssl);
        mbedtls_net_free(&s->client_fd);
        s->active = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Controller completed the TLS handshake");
    // Availability is published once the controller identifies itself in
    // its first UP frame, because only then is it known which device this
    // session belongs to.
    ESP_LOGI(TAG, "Free internal heap after controller handshake: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Only reach out to the vendor cloud when forwarding is switched on.
    // With it off the bridge answers the controller itself, so no
    // controller data ever leaves the local network.
    s->cloud_connected = false;
    if (wan_gate_cloud_forward()) {
        s->cloud_connected = connect_upstream(s);
        if (!s->cloud_connected) {
            ESP_LOGW(TAG, "Cloud unreachable — continuing local-only for "
                          "this session");
        }
    } else {
        ESP_LOGI(TAG, "Cloud mirroring off: answering the controller "
                      "directly, nothing is relayed to Spider Farmer");
    }

    handle_session(s);

    ESP_LOGI(TAG, "Session ended, free internal heap: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    vTaskDelete(NULL);
}

static void proxy_listener_task(void *arg)
{
    mbedtls_net_context listen_fd;
    mbedtls_net_init(&listen_fd);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", SB_LOCAL_TLS_PORT);
    int rc = mbedtls_net_bind(&listen_fd, NULL, port_str, MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not bind port %d: -0x%04x", SB_LOCAL_TLS_PORT, -rc);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TLS MITM proxy listening on port %d", SB_LOCAL_TLS_PORT);

    for (;;) {
        // Find a free slot. When all are busy, wait rather than accept a
        // connection that cannot be served.
        proxy_session_t *sess = NULL;
        for (int i = 0; i < SB_MAX_SESSIONS; i++) {
            if (!s_sessions[i].active) { sess = &s_sessions[i]; break; }
        }
        if (!sess) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        mbedtls_net_context client_fd;
        mbedtls_net_init(&client_fd);
        uint8_t peer_ip[4] = {0};
        size_t peer_len = 0;
        rc = mbedtls_net_accept(&listen_fd, &client_fd, peer_ip, sizeof(peer_ip), &peer_len);
        if (rc != 0) continue;

        // Log every connection attempt, not just successful ones — a
        // controller whose TLS handshake fails would otherwise leave no
        // trace at all.
        ESP_LOGI(TAG, "Connection from %u.%u.%u.%u on port %d",
                 peer_ip[0], peer_ip[1], peer_ip[2], peer_ip[3], SB_LOCAL_TLS_PORT);

        size_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (heap < SB_MIN_HEAP_FOR_SESSION) {
            ESP_LOGW(TAG, "Refusing connection: only %u bytes of internal heap left",
                     (unsigned)heap);
            mbedtls_net_free(&client_fd);
            continue;
        }

        sess->client_fd = client_fd;
        mbedtls_ssl_init(&sess->client_ssl);
        rc = mbedtls_ssl_setup(&sess->client_ssl, &s_server_conf);
        if (rc != 0) {
            ESP_LOGE(TAG, "ssl_setup failed: -0x%04x (free internal heap: %u bytes)",
                     -rc, (unsigned)heap);
            mbedtls_ssl_free(&sess->client_ssl);
            mbedtls_net_free(&sess->client_fd);
            continue;
        }
        mbedtls_ssl_set_bio(&sess->client_ssl, &sess->client_fd,
                            mbedtls_net_send, mbedtls_net_recv, NULL);

        // The slot is taken now; the handshake runs in the session task.
        sess->closing = false;
        sess->active = true;

        if (xTaskCreate(session_task, "sb_session", SB_PROXY_TASK_STACK,
                        sess, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Could not start the session task");
            mbedtls_ssl_free(&sess->client_ssl);
            mbedtls_net_free(&sess->client_fd);
            sess->active = false;
        }
    }
}

void mitm_proxy_start(void)
{
    sb_prov_cfg_t prov;
    sb_prov_load(&prov);
    strncpy(s_device_id, prov.device_id, sizeof(s_device_id) - 1);
    s_device_id[sizeof(s_device_id) - 1] = '\0';

    // Certificates and keys are parsed here, on the caller's (main task)
    // stack, so the listener itself only accepts and needs little stack.
    tls_globals_init();
    for (int i = 0; i < SB_MAX_SESSIONS; i++) {
        s_sessions[i].lock = xSemaphoreCreateMutex();
        s_sessions[i].inject_q = xQueueCreate(INJECT_Q_LEN, sizeof(inject_item_t));
        strcpy(s_sessions[i].down_topic_prefix, "CB");
    }

    config_poll_start();
    xTaskCreate(proxy_listener_task, "mitm_proxy", SB_PROXY_LISTENER_STACK, NULL, 5, NULL);
}
