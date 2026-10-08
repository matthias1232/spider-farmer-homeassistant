#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "provisioning.h"
#include "wifi_apsta.h"
#include "sys_log.h"
#include "syslog_fwd.h"

// NOTE: nothing in this file may use ESP_LOGx. Every log line is fed back
// into syslog_fwd_push(), so logging from the sender would loop. Errors are
// recorded in s_st.last_error and shown on the status page instead.

#define QUEUE_LEN      24
#define MAX_BACKOFF_MS 60000

typedef struct {
    char text[SYSLOG_LINE_MAX];
} msg_t;

static QueueHandle_t s_q = NULL;
static TaskHandle_t  s_task = NULL;
static volatile bool s_on = false;
static volatile bool s_reconfig = false;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

// Settings, copied under the spinlock by the task.
static char s_host[64] = "";
static int  s_port = 514;
static int  s_proto = SYSLOG_PROTO_UDP;
static bool s_insecure = false;

static syslog_fwd_status_t s_st;

static void set_error(const char *e)
{
    portENTER_CRITICAL(&s_mux);
    strncpy(s_st.last_error, e, sizeof(s_st.last_error) - 1);
    s_st.last_error[sizeof(s_st.last_error) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);
}

// RFC 5424 severity from the ESP log level letter ("I (1234) tag: ...").
static int severity(char level)
{
    switch (level) {
        case 'E': return 3;
        case 'W': return 4;
        case 'I': return 6;
        default:  return 7;
    }
}

// --- connection state, owned by the task ---
static int       c_udp = -1;
static struct sockaddr_in c_dst;
static esp_tls_t *c_tls = NULL;
static char     *c_ca = NULL;

static void disconnect(void)
{
    if (c_udp >= 0) { close(c_udp); c_udp = -1; }
    if (c_tls) { esp_tls_conn_destroy(c_tls); c_tls = NULL; }
    free(c_ca); c_ca = NULL;
    portENTER_CRITICAL(&s_mux);
    s_st.connected = false;
    portEXIT_CRITICAL(&s_mux);
}

static bool connect_now(const char *host, int port, int proto, bool insecure)
{
    disconnect();

    if (proto == SYSLOG_PROTO_UDP) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
        struct addrinfo *res = NULL;
        char ps[8];
        snprintf(ps, sizeof(ps), "%d", port);
        if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) {
            set_error("Cannot resolve host");
            return false;
        }
        memcpy(&c_dst, res->ai_addr, sizeof(c_dst));
        freeaddrinfo(res);
        c_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (c_udp < 0) { set_error("No UDP socket"); return false; }
    } else {
        esp_tls_cfg_t cfg = { .timeout_ms = 8000 };
        if (proto == SYSLOG_PROTO_TCP) {
            cfg.is_plain_tcp = true;
        } else if (insecure) {
            // Encrypted, server identity not verified -- by explicit choice.
            // With no CA and no bundle attached, esp-tls skips verification
            // (CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY).
            cfg.skip_common_name = true;
        } else {
            c_ca = malloc(2048);
            if (c_ca && sb_prov_syslog_ca_load(c_ca, 2048) > 0) {
                cfg.cacert_pem_buf = (const unsigned char *)c_ca;
                cfg.cacert_pem_bytes = strlen(c_ca) + 1;
            } else {
                free(c_ca); c_ca = NULL;
                cfg.crt_bundle_attach = esp_crt_bundle_attach;
            }
        }
        c_tls = esp_tls_init();
        if (!c_tls) { set_error("Out of memory for the connection"); return false; }
        if (esp_tls_conn_new_sync(host, strlen(host), port, &cfg, c_tls) != 1) {
            set_error(proto == SYSLOG_PROTO_TLS ? "TLS connection failed"
                                                : "TCP connection failed");
            esp_tls_conn_destroy(c_tls);
            c_tls = NULL;
            free(c_ca); c_ca = NULL;
            return false;
        }
    }

    portENTER_CRITICAL(&s_mux);
    s_st.connected = true;
    s_st.reconnects++;
    s_st.last_error[0] = '\0';
    portEXIT_CRITICAL(&s_mux);
    return true;
}

static bool write_all(const char *p, int m)
{
    int off = 0;
    while (off < m) {
        int w = esp_tls_conn_write(c_tls, p + off, m - off);
        if (w > 0) { off += w; continue; }
        if (w == ESP_TLS_ERR_SSL_WANT_READ || w == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        return false;
    }
    return true;
}

// One RFC 5424 message: <PRI>1 TIMESTAMP HOST APP PROCID MSGID SD MSG
// (facility local0 = 16). Timestamp "-": the server stamps it, which is
// right even before the bridge's own clock is synced. app is
// "spiderbridge" for system lines and "mqtt" for MQTT packets.
static bool send_msg(int proto, const char *app, int sev, const char *text)
{
    size_t cap = strlen(text) + 96;
    char stack[SYSLOG_LINE_MAX + 96];
    char *msg = cap <= sizeof(stack) ? stack : malloc(cap);
    if (!msg) return true;                        // drop, keep the link
    int n = snprintf(msg, cap, "<%d>1 - %s %s - - - %s",
                     16 * 8 + sev, prov_bridge_name(), app, text);
    bool ok = true;
    if (n > 0) {
        if (n >= (int)cap) n = cap - 1;
        if (proto == SYSLOG_PROTO_UDP) {
            ok = sendto(c_udp, msg, n, 0, (struct sockaddr *)&c_dst, sizeof(c_dst)) == n;
        } else {
            // RFC 6587 octet counting: "LEN SP MSG", so line breaks inside
            // a message cannot split it.
            char len[12];
            int l = snprintf(len, sizeof(len), "%d ", n);
            ok = write_all(len, l) && write_all(msg, n);
        }
    }
    if (msg != stack) free(msg);
    return ok;
}

static bool send_line(int proto, const char *text)
{
    return send_msg(proto, "spiderbridge", severity(text[0]), text);
}

// --- MQTT log copy ---
//
// Separate queue of heap strings: MQTT payloads are far longer than a log
// line. Bounded by count and total bytes, and skipped when the heap is
// low, so a chatty controller can never starve the TLS sessions.
#define MQTT_Q_LEN        12
#define MQTT_Q_MAX_BYTES  8192
#define MQTT_MIN_HEAP     45000
static QueueHandle_t s_mq = NULL;
static volatile int  s_mqtt_mode = 0;
static volatile int  s_mq_bytes = 0;

static void task(void *arg)
{
    (void)arg;
    msg_t m;
    bool have = false;          // m holds a line not yet sent
    int backoff = 1000;
    int64_t retry_at = 0;
    char host[64]; int port = 514, proto = 0; bool insecure = false;

    for (;;) {
        if (s_reconfig || !s_on) {
            disconnect();
            portENTER_CRITICAL(&s_mux);
            strcpy(host, s_host);
            port = s_port; proto = s_proto; insecure = s_insecure;
            s_reconfig = false;
            portEXIT_CRITICAL(&s_mux);
            backoff = 1000;
            retry_at = 0;
        }

        // MQTT copies first while the link is up; they are already bounded.
        char *mq = NULL;
        bool up_now = (proto == SYSLOG_PROTO_UDP) ? c_udp >= 0 : c_tls != NULL;
        if (s_mq && up_now && xQueueReceive(s_mq, &mq, 0) == pdTRUE && mq) {
            int len = (int)strlen(mq);
            bool ok = s_on && host[0] ? send_msg(proto, "mqtt", 7, mq) : true;
            portENTER_CRITICAL(&s_mux);
            s_mq_bytes -= len;
            if (ok) s_st.sent++; else s_st.dropped++;
            portEXIT_CRITICAL(&s_mux);
            free(mq);
            if (!ok) {
                set_error("Connection lost");
                disconnect();
                retry_at = esp_timer_get_time() + 1000LL * 1000;
            }
            continue;
        }

        if (!have) {
            if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(s_mq && uxQueueMessagesWaiting(s_mq) ? 0 : 250)) != pdTRUE) {
                // Nothing on the system queue: an MQTT copy waiting while the
                // link is down still needs a connection, so treat it as work.
                if (!(s_mq && uxQueueMessagesWaiting(s_mq))) continue;
                if (!up_now) {
                    if (!wifi_apsta_uplink_is_up() || esp_timer_get_time() < retry_at) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                        continue;
                    }
                    if (!s_on || !host[0]) continue;
                    if (!connect_now(host, port, proto, insecure)) {
                        retry_at = esp_timer_get_time() + (int64_t)backoff * 1000;
                        backoff = backoff * 2 > MAX_BACKOFF_MS ? MAX_BACKOFF_MS : backoff * 2;
                    } else {
                        backoff = 1000;
                    }
                }
                continue;
            }
            have = true;
        }
        if (!s_on || !host[0]) { have = false; continue; }

        bool up = (proto == SYSLOG_PROTO_UDP) ? c_udp >= 0 : c_tls != NULL;
        if (!up) {
            // No network calls before the uplink has an address: DNS and
            // connect would only fail (or, very early in boot, assert).
            if (!wifi_apsta_uplink_is_up()) {
                set_error("Waiting for the network");
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            if (esp_timer_get_time() < retry_at) {
                // Waiting out the backoff: the queue keeps filling and drops
                // the overflow, so nothing here blocks the logging tasks.
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            if (!connect_now(host, port, proto, insecure)) {
                retry_at = esp_timer_get_time() + (int64_t)backoff * 1000;
                backoff = backoff * 2 > MAX_BACKOFF_MS ? MAX_BACKOFF_MS : backoff * 2;
                continue;
            }
            backoff = 1000;
        }

        if (send_line(proto, m.text)) {
            portENTER_CRITICAL(&s_mux);
            s_st.sent++;
            portEXIT_CRITICAL(&s_mux);
            have = false;
        } else {
            // Keep the line and reconnect; a stream that broke mid-write
            // would otherwise lose it.
            set_error("Connection lost");
            disconnect();
            retry_at = esp_timer_get_time() + 1000LL * 1000;
        }
    }
}

void syslog_fwd_configure(const char *host, int port, int proto, bool insecure)
{
    portENTER_CRITICAL(&s_mux);
    strncpy(s_host, host ? host : "", sizeof(s_host) - 1);
    s_host[sizeof(s_host) - 1] = '\0';
    s_port = (port > 0 && port < 65536) ? port
           : (proto == SYSLOG_PROTO_TLS ? 6514 : 514);
    s_proto = (proto >= 0 && proto <= 2) ? proto : 0;
    s_insecure = insecure;
    strcpy(s_st.host, s_host);
    s_st.port = s_port;
    s_st.proto = s_proto;
    s_st.enabled = s_host[0] != '\0';
    s_st.last_error[0] = '\0';
    s_reconfig = true;
    portEXIT_CRITICAL(&s_mux);

    if (s_host[0] && !s_q) {
        s_q = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
        // TLS needs room for the handshake; 6 KB covers mbedTLS on this
        // build with the asymmetric 2 KB / 1 KB record buffers.
        if (s_q && xTaskCreate(task, "sb_syslog", 6144, NULL, 2, &s_task) != pdPASS) {
            vQueueDelete(s_q);
            s_q = NULL;
        }
    }
    s_on = s_host[0] && s_q;
}

void syslog_fwd_start(void)
{
    sb_prov_cfg_t *p = malloc(sizeof(*p));
    if (!p) return;
    sb_prov_load(p);
    syslog_fwd_configure(p->syslog_host, p->syslog_port, p->syslog_proto,
                         p->syslog_tls_insecure);
    syslog_fwd_set_mqtt(p->syslog_mqtt);
    free(p);
}

void syslog_fwd_push(const char *line)
{
    if (!s_on || !s_q || !line || !line[0]) return;
    // The sender's own task produces no log lines (see the note at the top),
    // but esp-tls / lwIP underneath it can. Those are dropped rather than
    // queued, so a failing connection cannot feed itself.
    if (xTaskGetCurrentTaskHandle() == s_task) return;
    msg_t m;
    strncpy(m.text, line, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = '\0';
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_mux);
        s_st.dropped++;
        portEXIT_CRITICAL(&s_mux);
    }
}

void syslog_fwd_set_mqtt(int mode)
{
    s_mqtt_mode = (mode >= 0 && mode <= 2) ? mode : 0;
    if (s_mqtt_mode && !s_mq) s_mq = xQueueCreate(MQTT_Q_LEN, sizeof(char *));
}

// Directions as in live_log.h: 0 UP, 1 DOWN, 2 HA_OUT, 3 HA_IN.
void syslog_fwd_push_mqtt(int dir, const char *type, const char *topic,
                          const uint8_t *payload, size_t len)
{
    int mode = s_mqtt_mode;
    if (!mode || !s_on || !s_mq) return;
    bool ha = dir >= 2;
    if (ha && mode < 2) return;
    if (xTaskGetCurrentTaskHandle() == s_task) return;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < MQTT_MIN_HEAP) {
        portENTER_CRITICAL(&s_mux);
        s_st.dropped++;
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    static const char *const DIRS[] = { "UP", "DOWN", "HA OUT", "HA IN" };
    size_t tl = topic ? strlen(topic) : 0;
    if (len > 4096) len = 4096;
    size_t need = 40 + tl + len;
    if (s_mq_bytes + (int)need > MQTT_Q_MAX_BYTES) {
        portENTER_CRITICAL(&s_mux);
        s_st.dropped++;
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    char *s = malloc(need);
    if (!s) return;
    int n = snprintf(s, need, "%s %s %s ", DIRS[dir & 3], type ? type : "", topic ? topic : "");
    // Non-printable bytes become '.', so the payload stays one syslog line.
    for (size_t i = 0; i < len && n + 1 < (int)need; i++) {
        uint8_t c = payload[i];
        s[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    s[n] = '\0';
    int sz = (int)strlen(s);
    portENTER_CRITICAL(&s_mux);
    s_mq_bytes += sz;
    portEXIT_CRITICAL(&s_mux);
    if (xQueueSend(s_mq, &s, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_mux);
        s_mq_bytes -= sz;
        s_st.dropped++;
        portEXIT_CRITICAL(&s_mux);
        free(s);
    }
}

void syslog_fwd_status(syslog_fwd_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}
