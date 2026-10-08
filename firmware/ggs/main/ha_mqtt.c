#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "sb_config.h"
#include "provisioning.h"
#include "mitm_proxy.h"
#include "sf_command_handler.h"
#include "sf_normalizer.h"
#include "config_poll.h"
#include "plan_templates.h"
#include "plan_store.h"
#include "plan_text.h"
#include "sf_alarm.h"
#include "device_cache.h"
#include "device_registry.h"
#include "time_sync.h"
#include "wifi_apsta.h"
#include "sb_system.h"
#include "tz_table.h"
#include "wan_gate.h"
#include "dns_hijack.h"
#include "esp_heap_caps.h"
#include "web_auth.h"
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_random.h"
#include "ha_discovery_table.h"
#include "ha_discovery_stale.h"
#include "live_log.h"
#include "ha_mqtt.h"

static const char *TAG = "ha_mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static char s_device_id[32];
static char s_raw_topic[64];
static int  s_publish_mode = SB_PUBLISH_MODE_DEFAULT;
static bool s_connected = false;

// Enough detail to tell "never configured" from "configured but
// refused", which look identical from the outside and need opposite
// fixes. The last error is kept verbatim because a wrong password and an
// unreachable host both just show as "not connected" otherwise.
static char     s_broker_uri[128] = "";
static bool     s_configured = false;
static uint32_t s_connect_count = 0;
static uint32_t s_disconnect_count = 0;
static uint32_t s_publish_count = 0;
static char     s_last_error[96] = "";
static int64_t  s_connected_since_us = 0;

// --- Connection history, for chasing a flapping session ---
//
// The syslog ring holds 30 lines and the MQTT flood can consume them in a
// second, which is exactly when the history is needed: a session that dies
// every ten seconds leaves no trace in either counter once the numbers
// themselves look normal. Each transition is stamped so a burst can be
// matched against what else happened that moment (heap, publishes), and
// the transport errno — a RST, a timeout and a refused TCP all read as
// "not connected" from the outside.
//
// Entries are appended from the MQTT event task, so the lock protects a
// few dozen bytes of memcpy and readers are the web handlers.
typedef struct {
    uint32_t seq;
    uint32_t ms;            // ms since boot, matching sys_log stamps
    char     what;          // 'c' connect, 'd' disconnect, 'e' error
    uint16_t detail;        // error_type or errno depending on 'what'
    uint16_t payload;       // sub-detail: server rc or socket errno
    uint32_t publishes;     // s_publish_count at that moment
} local_hist_t;

#define MQTT_HIST_ENTRIES 16
static local_hist_t s_hist[MQTT_HIST_ENTRIES];
static int         s_hist_head = 0;
static int         s_hist_count = 0;
static uint32_t    s_hist_seq = 0;
static portMUX_TYPE s_hist_mux = portMUX_INITIALIZER_UNLOCKED;

static void hist_add(char what, uint16_t detail, uint16_t payload)
{
    portENTER_CRITICAL(&s_hist_mux);
    local_hist_t *e = &s_hist[s_hist_head];
    e->seq = ++s_hist_seq;
    e->ms = (uint32_t)(esp_timer_get_time() / 1000);
    e->what = what;
    e->detail = detail;
    e->payload = payload;
    e->publishes = s_publish_count;
    s_hist_head = (s_hist_head + 1) % MQTT_HIST_ENTRIES;
    if (s_hist_count < MQTT_HIST_ENTRIES) s_hist_count++;
    portEXIT_CRITICAL(&s_hist_mux);
}

// Reads the ring oldest-first for the status page.
int ha_mqtt_history(ha_mqtt_hist_t *out, int max)
{
    int n = 0;
    portENTER_CRITICAL(&s_hist_mux);
    int start = (s_hist_head - s_hist_count + MQTT_HIST_ENTRIES) %
                MQTT_HIST_ENTRIES;
    for (int i = 0; i < s_hist_count && n < max; i++) {
        const local_hist_t *h = &s_hist[(start + i) % MQTT_HIST_ENTRIES];
        out[n].seq = h->seq;
        out[n].ms = h->ms;
        out[n].what = h->what;
        out[n].detail = h->detail;
        out[n].payload = h->payload;
        out[n].publishes = h->publishes;
        n++;
    }
    portEXIT_CRITICAL(&s_hist_mux);
    return n;
}

static void publish_bridge_discovery(void);
static void clear_legacy_discovery(void);
static void state_cache_clear(void);

// Refreshes the bridge's own state on a timer.
//
// Signal strength, free heap and uptime all change continuously; a value
// published once at connect would be stale within a minute and look like
// a live reading.
// Keeps every controller's sensor-cleaning countdown moving between
// controller frames. Unchanged values are suppressed in
// ha_mqtt_publish_state, so this only sends when a second has passed.
static void refresh_cleaning_countdowns(void)
{
    char macs[SB_MAX_DEVICES][DEV_MAC_LEN];
    char slugs[SB_MAX_DEVICES][DEV_SLUG_LEN];
    int n = 0;
    device_registry_lock();
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        device_entry_t *d = device_registry_at(i);
        if (!d) continue;
        strncpy(macs[n], d->mac, DEV_MAC_LEN - 1);   macs[n][DEV_MAC_LEN - 1] = '\0';
        strncpy(slugs[n], d->slug, DEV_SLUG_LEN - 1); slugs[n][DEV_SLUG_LEN - 1] = '\0';
        n++;
    }
    device_registry_unlock();
    for (int i = 0; i < n; i++) {
        int phase = 0;
        if (device_cache_sensor_cleaning_phase(macs[i], &phase, NULL) && phase != 0) {
            sf_publish_sensor_cleaning(macs[i], slugs[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Grow plan template choice (Home Assistant select + "Add stage" button)
//
// The select's options are the template names; the choice is kept per
// controller in RAM and echoed on its state topic. Names map back to ids.
// ---------------------------------------------------------------------------
static struct { char slug[DEV_SLUG_LEN]; char id[16]; } s_tpl_sel[SB_MAX_DEVICES];

static void tpl_name_for(const char *id, char *out, size_t n)
{
    plan_tpl_name(id, out, n);
    if (!out[0]) snprintf(out, n, "%s", id);
}

void ha_mqtt_publish_plan_template_sel(const char *slug)
{
    char id[16] = "sys:seedling";
    ha_mqtt_get_plan_template_sel(slug, id, sizeof(id));
    char name[48];
    tpl_name_for(id, name, sizeof(name));
    char topic[96];
    snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/plan/template_sel", slug);
    ha_mqtt_publish_state(topic, name);
}

void ha_mqtt_set_plan_template_sel(const char *slug, const char *value)
{
    // Accept a name (from the select) or an id.
    char id[16] = "";
    if (strncmp(value, "sys:", 4) == 0 || strncmp(value, "cust:", 5) == 0) {
        strncpy(id, value, sizeof(id) - 1);
    } else {
        char name[48];
        for (int i = 0; i < PLAN_TPL_SYSTEM && !id[0]; i++) {
            tpl_name_for(plan_tpl_system_id(i), name, sizeof(name));
            if (strcmp(name, value) == 0) strncpy(id, plan_tpl_system_id(i), sizeof(id) - 1);
        }
        for (int i = 0; i < PLAN_TPL_CUSTOM && !id[0]; i++) {
            char cid[16];
            snprintf(cid, sizeof(cid), "cust:%d", i);
            plan_tpl_name(cid, name, sizeof(name));
            if (name[0] && strcmp(name, value) == 0) strcpy(id, cid);
        }
    }
    if (!id[0]) { ESP_LOGW(TAG, "Unknown plan template '%s'", value); return; }
    int free_i = -1;
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        if (strcmp(s_tpl_sel[i].slug, slug) == 0) { free_i = i; break; }
        if (free_i < 0 && !s_tpl_sel[i].slug[0]) free_i = i;
    }
    if (free_i < 0) free_i = 0;
    strncpy(s_tpl_sel[free_i].slug, slug, sizeof(s_tpl_sel[free_i].slug) - 1);
    strncpy(s_tpl_sel[free_i].id, id, sizeof(s_tpl_sel[free_i].id) - 1);
    ha_mqtt_publish_plan_template_sel(slug);
}

// The plan template select: its options are the current template names
// (system and custom), so it is built here rather than taken from the
// static table, and re-sent on its own when the templates change -- a full
// rediscovery (~150 messages) for one select filled the outbox and lost
// the availability topic.
#define TPL_SELECT_MAX_OPTIONS 40
bool ha_mqtt_publish_plan_template_select(const char *slug)
{
    if (!s_client || !slug || !slug[0]) return false;
    size_t psz = 2048;
    char *payload = malloc(psz);
    char *topic = malloc(160);
    if (!payload || !topic) { free(payload); free(topic); return false; }
    int o = snprintf(payload, psz,
        "{\"availability\":[{\"topic\":\"spiderfarmer/bridge/availability\"},"
        "{\"topic\":\"spiderfarmer/%s/availability\"}],\"availability_mode\":\"all\","
        "\"device\":{\"identifiers\":[\"spiderfarmer_%s\"]},"
        "\"name\":\"Plan Template\",\"icon\":\"mdi:file-document-multiple-outline\","
        "\"entity_category\":\"config\",\"unique_id\":\"spiderfarmer_%s_plan_template_sel\","
        "\"command_topic\":\"spiderfarmer/%s/command/plan/template_sel/set\","
        "\"state_topic\":\"spiderfarmer/%s/state/plan/template_sel\",\"options\":[",
        slug, slug, slug, slug, slug);
    int count = 0;
    char id[16], name[48];
    for (int i = 0; i < PLAN_TPL_SYSTEM + PLAN_TPL_CUSTOM && count < TPL_SELECT_MAX_OPTIONS &&
                    o > 0 && o < (int)psz - 64; i++) {
        if (i < PLAN_TPL_SYSTEM) snprintf(id, sizeof(id), "%s", plan_tpl_system_id(i));
        else snprintf(id, sizeof(id), "cust:%d", i - PLAN_TPL_SYSTEM);
        plan_tpl_name(id, name, sizeof(name));
        if (!name[0]) continue;
        o += snprintf(payload + o, psz - o, "%s\"%s\"", count++ ? "," : "", name);
    }
    bool ok = false;
    if (o > 0 && o < (int)psz - 4) {
        snprintf(payload + o, psz - o, "]}");
        snprintf(topic, 160, "homeassistant/select/spiderfarmer_%s_plan_template_sel/config", slug);
        ok = esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1) >= 0;
        ha_mqtt_publish_plan_template_sel(slug);
    }
    free(payload);
    free(topic);
    return ok;
}

void ha_mqtt_refresh_plan_template_selects(void)
{
    if (!s_client || !s_connected || !ha_mqtt_ha_enabled()) return;
    char slugs[SB_MAX_DEVICES][DEV_SLUG_LEN];
    int n = 0;
    device_registry_lock();
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        device_entry_t *d = device_registry_at(i);
        if (!d) continue;
        strncpy(slugs[n], d->slug, DEV_SLUG_LEN - 1);
        slugs[n][DEV_SLUG_LEN - 1] = '\0';
        n++;
    }
    device_registry_unlock();
    for (int i = 0; i < n; i++) ha_mqtt_publish_plan_template_select(slugs[i]);
}

void ha_mqtt_get_plan_template_sel(const char *slug, char *out, size_t n)
{
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        if (s_tpl_sel[i].slug[0] && strcmp(s_tpl_sel[i].slug, slug) == 0) {
            snprintf(out, n, "%s", s_tpl_sel[i].id);
            return;
        }
    }
    snprintf(out, n, "sys:seedling");
}

static volatile bool s_avail_retry = false;
static void publish_all_availability(void);

// The bridge's own availability and every controller's, retained. A -2
// (outbox full) or -1 return sets the retry flag for the next second.
static void republish_availability(void)
{
    if (!s_client || !s_connected) return;
    s_avail_retry = false;
    if (esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/availability",
                                "online", 0, 0, 1) < 0) {
        s_avail_retry = true;
        return;
    }
    if (ha_mqtt_ha_enabled()) publish_all_availability();
}

// Once a second, on the config-poll task's hook (no task of its own).
// First publish 3 s after connecting, once discovery has drained; then the
// diagnostics every 30 s and the cleaning countdown every second while a
// cycle or cooldown runs.
static char s_heap_low_note[160];
const char *ha_mqtt_heap_low_note(void) { return s_heap_low_note; }

static void bridge_state_hook(void)
{
    // A new all-time heap low below 25 KB is logged with what was going on
    // (sessions, web), so the cause of a dip can be found in the log
    // afterwards. At most once a minute.
    {
        static size_t last_low = SIZE_MAX;
        static int64_t last_log_us = 0;
        size_t low = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        int64_t now = esp_timer_get_time();
        if (low < 25 * 1024 && low < last_low && now - last_log_us > 60LL * 1000000) {
            uint32_t served, shed, busy;
            web_stats(&served, &shed, &busy);
            snprintf(s_heap_low_note, sizeof(s_heap_low_note),
                     "%u B at uptime %lld s (largest block now %u B; cloud link %s; web served %lu, shed %lu)",
                     (unsigned)low, (long long)(now / 1000000),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     mitm_proxy_cloud_reconnecting() ? "reconnecting" : "steady",
                     (unsigned long)served, (unsigned long)shed);
            ESP_LOGW(TAG, "New heap low %s", s_heap_low_note);
            last_low = low;
            last_log_us = now;
        }
    }
    static int since_connect = -1;
    if (!s_connected) { since_connect = -1; return; }
    since_connect++;
    if (since_connect < 3) return;
    if ((since_connect - 3) % 30 == 0) {
        ha_mqtt_publish_bridge_state();
        // Availability is retained, but a publish lost while the outbox
        // was full (the discovery burst right after connecting) left the
        // broker's last-will "offline" in place and Home Assistant showed
        // every controller unavailable although it was connected. Sent
        // again every 30 s, so a lost one heals on its own.
        republish_availability();
    } else if (s_avail_retry) {
        republish_availability();
    }
    if (ha_mqtt_ha_enabled()) refresh_cleaning_countdowns();
}

bool ha_mqtt_ha_enabled(void)   { return (s_publish_mode & SB_PUB_HA) != 0; }
bool ha_mqtt_raw_enabled(void)  { return (s_publish_mode & SB_PUB_RAW) != 0; }
bool ha_mqtt_is_connected(void) { return s_connected; }

// ---------------------------------------------------------------------------
// Incoming commands
// ---------------------------------------------------------------------------

// Topic layout: spiderfarmer/<id>/command/<field>[/<subfield>]/set
// plus the raw escape hatch:  spiderfarmer/<id>/command/raw
static void handle_command(const char *topic, const char *payload, int payload_len)
{
    char buf[160];
    strncpy(buf, topic, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *parts[8];
    int n = 0;
    char *tok = strtok(buf, "/");
    while (tok && n < 8) { parts[n++] = tok; tok = strtok(NULL, "/"); }

    if (n < 4 || strcmp(parts[2], "command") != 0) return;

    // "bridge" addresses the bridge itself rather than a controller:
    // its own clock settings, which no controller reports.
    if (strcmp(parts[1], "bridge") == 0) {
        char value[128];
        int vlen = payload_len < (int)sizeof(value) - 1
                   ? payload_len : (int)sizeof(value) - 1;
        memcpy(value, payload, vlen);
        value[vlen] = '\0';

        bool ok = false;
        const char *what = parts[3];
        if (strcmp(what, "timezone") == 0) {
            // A zone name from the dropdown or the text entity. The
            // POSIX rules come from the table, so a name is enough and a
            // typo is caught rather than applied.
            const char *posix = tz_posix_for(value);
            if (!posix) {
                ESP_LOGW(TAG, "Unknown time zone '%s' — ignored", value);
                ha_mqtt_publish_bridge_state();
                return;
            }

            sb_prov_cfg_t *pc = malloc(sizeof(*pc));
            if (pc) {
                sb_prov_load(pc);
                strncpy(pc->tz_name, value, sizeof(pc->tz_name) - 1);
                pc->tz_name[sizeof(pc->tz_name) - 1] = '\0';
                strncpy(pc->tz, posix, sizeof(pc->tz) - 1);
                pc->tz[sizeof(pc->tz) - 1] = '\0';
                sb_prov_save(pc);
                free(pc);

                time_sync_apply_tz();
                if (prov_tz_push()) sf_push_timezone_all();
                ESP_LOGI(TAG, "Bridge time zone set to %s (%s)", value, posix);
                ok = true;
            }
        } else if (strcmp(what, "dst") == 0) {
            ok = time_sync_set_dst_mode(value);
        } else if (strcmp(what, "ntp_server") == 0) {
            ok = time_sync_set_ntp_server(value);
        } else if (strcmp(what, "dst_on") == 0) {
            // Pins the clock in the chosen direction. Turning this
            // either way means the zone rules no longer decide, so
            // "follow the rules" switches itself off.
            prov_set_dst_mode(strcmp(value, "ON") == 0 ? SB_DST_SUMMER
                                                       : SB_DST_STANDARD);
            time_sync_apply_tz();
            if (prov_tz_push()) sf_push_timezone_all();
            ok = true;
        } else if (strcmp(what, "dst_auto") == 0) {
            if (strcmp(value, "ON") == 0) {
                prov_set_dst_mode(SB_DST_AUTO);
            } else {
                // Leaving automatic has to land somewhere, so pin it to
                // whatever is in force right now. Anything else would
                // change the clock as a side effect of a setting that
                // only meant "stop switching".
                time_status_t t;
                time_sync_get_status(&t);
                prov_set_dst_mode(t.is_dst ? SB_DST_SUMMER : SB_DST_STANDARD);
            }
            time_sync_apply_tz();
            if (prov_tz_push()) sf_push_timezone_all();
            ok = true;
        } else if (strcmp(what, "tz_push") == 0 ||
                   strcmp(what, "wan") == 0 ||
                   strcmp(what, "cloudfw") == 0 ||
                   strcmp(what, "dns_ex") == 0 ||
                   strcmp(what, "ntp_ex") == 0 ||
                   strcmp(what, "dns_rd") == 0 ||
                   strcmp(what, "ntp_rd") == 0) {
            // The network switches. All stored the same way, and all
            // applied without a reboot.
            bool on = (strcmp(value, "ON") == 0);
            sb_prov_cfg_t *pc = malloc(sizeof(*pc));
            if (pc) {
                sb_prov_load(pc);

                if      (strcmp(what, "tz_push") == 0) pc->tz_push = on;
                else if (strcmp(what, "wan") == 0)     pc->wan_open = on;
                else if (strcmp(what, "cloudfw") == 0) pc->cloud_forward = on;
                else if (strcmp(what, "dns_ex") == 0)  pc->allow_dns_offline = on;
                else if (strcmp(what, "ntp_ex") == 0)  pc->allow_ntp_offline = on;
                else if (strcmp(what, "dns_rd") == 0)  pc->dns_redirect = on;
                else if (strcmp(what, "ntp_rd") == 0)  pc->ntp_redirect = on;

                sb_prov_save(pc);

                // Apply at once rather than at the next restart.
                wan_gate_set(pc->wan_open);
                wan_gate_set_cloud_forward(pc->cloud_forward);
                dns_set_offline_exceptions(pc->allow_dns_offline,
                                           pc->allow_ntp_offline);
                dns_set_ntp_target(pc->ntp_target, pc->ntp_redirect);
                dns_set_target(pc->dns_target, pc->dns_redirect);

                if (strcmp(what, "tz_push") == 0 && on) {
                    sf_push_timezone_all();
                }
                free(pc);
                ESP_LOGI(TAG, "%s set to %s", what, on ? "on" : "off");
                ok = true;
            }
        } else if (strcmp(what, "bridge_name") == 0) {
            ha_mqtt_rename_bridge(value);
            return;
        } else if (strcmp(what, "reboot") == 0) {
            ESP_LOGW(TAG, "Restart requested over MQTT");
            sb_system_reboot(1200);
            return;
        } else if (strcmp(what, "factory_reset") == 0) {
            // Only the exact word acts. A text entity rather than a
            // button precisely so a mis-click or a stray automation
            // cannot discard the WiFi credentials and every device name.
            if (strcmp(value, "RESET") == 0) {
                ESP_LOGW(TAG, "Factory reset requested over MQTT");
                sb_system_factory_reset();
                return;
            }
            ESP_LOGW(TAG, "Factory reset ignored: expected RESET, got '%s'",
                     value);
            esp_mqtt_client_publish(s_client,
                                    "spiderfarmer/bridge/state/reset_hint",
                                    "type RESET", 0, 0, 1);
            return;
        } else {
            ESP_LOGW(TAG, "Unknown bridge command '%s'", what);
        }

        if (ok) ha_mqtt_publish_bridge_state();
        return;
    }

    // parts[1] is the slug the user chose. Map it back to the MAC, which
    // is what the controller and the cloud actually use — the rename only
    // ever exists on the Home Assistant side.
    const char *slug = parts[1];
    char dev_mac[DEV_MAC_LEN] = {0};

    device_registry_lock();
    device_entry_t *dev = device_registry_find_by_slug(slug);
    if (dev) {
        strncpy(dev_mac, dev->mac, sizeof(dev_mac) - 1);
    }
    device_registry_unlock();

    if (!dev_mac[0]) {
        ESP_LOGW(TAG, "Command for unknown device '%s' — ignored", slug);
        return;
    }

    // Raw passthrough: whatever Home Assistant sends goes straight into
    // that controller's session, unmodified.
    if (strcmp(parts[3], "raw") == 0) {
        mitm_proxy_inject_command_to(dev_mac, payload, (size_t)payload_len);
        return;
    }

    if (!ha_mqtt_ha_enabled()) return;

    const char *field = parts[3];
    // .../command/<field>/set        -> n == 5, no subfield
    // .../command/<field>/<sub>/set  -> n == 6
    const char *subfield = (n >= 6 && strcmp(parts[4], "set") != 0) ? parts[4] : NULL;

    char value[256];
    int vlen = payload_len < (int)sizeof(value) - 1 ? payload_len : (int)sizeof(value) - 1;
    memcpy(value, payload, vlen);
    value[vlen] = '\0';

    const char *mac = dev_mac;
    const char *uid = mitm_proxy_uid_for(dev_mac);
    if (!mitm_proxy_device_online(dev_mac)) {
        ESP_LOGW(TAG, "Command for %s but '%s' is not connected", field, slug);
        return;
    }

    // The zone entity sends a name; the command needs "Name|POSIX".
    //
    // Accepted even while the bridge manages the clock. The change is
    // applied, the sync notices the mismatch and puts it back, and the
    // state topic then reports what is actually in force — so Home
    // Assistant shows the correction rather than a rejection. A control
    // that works and is overridden is easier to understand than one
    // that refuses with an explanation nobody reads.
    if (strcmp(field, "timezone") == 0) {
        const char *posix = tz_posix_for(value);
        if (!posix) {
            ESP_LOGW(TAG, "Unknown time zone '%s'", value);
            return;
        }
        // Rebuilt in place. A zone name is well under 40 characters and
        // the rules under 56, so this always fits the value buffer.
        char name[40];
        strncpy(name, value, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        snprintf(value, sizeof(value), "%s|%s", name, posix);
    }

    // Range alarms have a third level: .../command/alarm/temp/max/set
    // (n == 7). It is passed on as "max=<value>", the form the alarm
    // command builder takes.
    if (strcmp(field, "alarm") == 0 && n >= 7 && strcmp(parts[6], "set") == 0) {
        // Values here are short numbers or ON/OFF; anything longer is not
        // an alarm value and is refused rather than cut.
        char tmp[64];
        int w = snprintf(tmp, sizeof(tmp), "%.15s=%.40s", parts[5], value);
        if (w <= 0 || w >= (int)sizeof(tmp)) return;
        memcpy(value, tmp, (size_t)w + 1);
    }

    // Device time: setDevTimezone with the bridge's own zone and clock.
    if (strcmp(field, "time_sync") == 0) {
        if (!sf_sync_device_time(dev_mac, uid))
            ESP_LOGW(TAG, "Time sync for %s not sent (bridge clock not set?)", dev_mac);
        return;
    }

    // Grow plan template choice: kept in RAM for the "Add stage" button;
    // nothing is sent to the controller.
    if (strcmp(field, "plan") == 0 && subfield && strcmp(subfield, "template_sel") == 0) {
        ha_mqtt_set_plan_template_sel(slug, value);
        return;
    }

    // Grow plan: built as text (a five-stage plan as a cJSON tree would
    // not fit in the heap).
    if (strcmp(field, "plan") == 0 || strcmp(field, "plan_template") == 0) {
        char err[128] = "unknown template or clock not set";
        const char *v = value;
        char sel[24];
        if (strcmp(field, "plan_template") == 0 &&
            (strcasecmp(value, "PRESS") == 0 || strcasecmp(value, "selected") == 0 || !value[0])) {
            ha_mqtt_get_plan_template_sel(slug, sel, sizeof(sel));
            v = sel;
        }
        // A long plan kept on the bridge: the same buttons act on it.
        plan_store_info_t li;
        if (SB_LONG_PLAN && plan_store_info(dev_mac, &li) && li.active) {
            bool ok = false;
            if (strcmp(field, "plan_template") == 0) {
                int start = sf_plan_today_packed();
                if (li.count) {
                    char *last = malloc(PLAN_STORE_STAGE_MAX + 8);
                    if (last && plan_store_get(dev_mac, li.count - 1, last, PLAN_STORE_STAGE_MAX + 8)) {
                        long e = jtext_int(last, strlen(last), "endDate", 0);
                        if (e) start = sf_plan_date_add_days((int)e, 1);
                    }
                    free(last);
                }
                char id[24];
                snprintf(id, sizeof(id), "%s%.18s", strchr(v, ':') ? "" : "sys:", v);
                char *stage = start ? plan_tpl_make_stage(id, start, 0) : NULL;
                ok = stage && plan_store_put(dev_mac, stage, err, sizeof(err)) >= 0;
                free(stage);
            } else if (subfield && strcmp(subfield, "enabled") == 0) {
                bool on = strcasecmp(value, "ON") == 0 || strcmp(value, "1") == 0;
                plan_store_set_running(dev_mac, on);
                char *cmdt = sf_plan_command_text("plan", "enabled", on ? "ON" : "OFF", mac, uid, err, sizeof(err));
                if (cmdt) { mitm_proxy_inject_command_to(dev_mac, cmdt, strlen(cmdt)); free(cmdt); }
                sf_publish_plan(dev_mac, slug);
                return;
            } else if (subfield && strcmp(subfield, "delete") == 0) {
                ok = plan_store_delete(dev_mac, atol(value));
            }
            if (ok) {
                int today = sf_plan_today_packed();
                int cur = today ? plan_store_current(dev_mac, today) : 0;
                if (cur >= 0) sf_plan_window_push(dev_mac, uid, cur);
            } else {
                ESP_LOGW(TAG, "Bridge plan command %s/%s not applied: %s", field, subfield ? subfield : "-", err);
            }
            sf_publish_plan(dev_mac, slug);
            return;
        }
        char *cmdt = sf_plan_command_text(field, subfield, v, mac, uid, err, sizeof(err));
        if (!cmdt) {
            ESP_LOGW(TAG, "Plan command %s/%s not sent: %s", field, subfield ? subfield : "-", err);
            return;
        }
        mitm_proxy_inject_command_to(dev_mac, cmdt, strlen(cmdt));
        free(cmdt);
        sf_publish_plan(dev_mac, slug);
        return;
    }

    cJSON *cmd = sf_translate_command(field, subfield, value, mac, uid);
    if (!cmd) {
        ESP_LOGW(TAG, "Could not translate command %s/%s = %s",
                 field, subfield ? subfield : "-", value);
        return;
    }

    char *json = cJSON_PrintUnformatted(cmd);
    if (json) {
        mitm_proxy_inject_command_to(dev_mac, json, strlen(json));

        // Sensor cleaning has no module block to merge, so its switch
        // state is echoed straight back. Without this Home Assistant's
        // switch had nothing to confirm the command and flipped back.
        if ((strcmp(field, "sensor_cleaning") == 0 || strcmp(field, "sensor_heating") == 0)) {
            sf_publish_sensor_cleaning(dev_mac, slug);
        }

        // The alarm block that was sent is now the controller's: cached
        // and echoed, so Home Assistant's control does not spring back.
        if (strcmp(field, "alarm") == 0) {
            cJSON *p = cJSON_GetObjectItem(cmd, "params");
            cJSON *a = p ? cJSON_GetObjectItem(p, "alarm") : NULL;
            if (cJSON_IsObject(a)) {
                device_cache_replace(dev_mac, "alarm", a);
                sf_publish_alarm(dev_mac, slug);
            }
        }

        // Optimistic update: write the new block into the cache and
        // republish the derived state topics right away, instead of
        // waiting for the controller's next status frame.
        //
        // Without this a Home Assistant toggle visibly springs back
        // after being clicked. For light modes it is essential: some
        // firmware versions never report modeType in status frames, so
        // the dropdown would stay wrong indefinitely.
        cJSON *params = cJSON_GetObjectItem(cmd, "params");
        if (cJSON_IsObject(params)) {
            const char *modules[] = { "fan", "blower", "light", "light2" };
            for (int i = 0; i < 4; i++) {
                cJSON *blk = cJSON_GetObjectItem(params, modules[i]);
                if (!cJSON_IsObject(blk)) continue;

                if (cJSON_GetObjectItem(blk, "modeType")) device_cache_note_mode_command(dev_mac, modules[i]);
                device_cache_merge(dev_mac, modules[i], blk);

                // Copy under the lock, publish without it (see
                // sf_publish_device_status).
                device_cache_lock();
                cJSON *c0 = device_cache_get(dev_mac, modules[i]);
                cJSON *cached = c0 ? cJSON_Duplicate(c0, true) : NULL;
                device_cache_unlock();
                if (i < 2) {
                    sf_publish_fan_state(slug, modules[i], cached ? cached : blk);
                    sf_publish_fan_speed(slug, modules[i], cached ? cached : blk);
                    sf_publish_fan_extras(slug, modules[i], cached ? cached : blk);
                } else {
                    sf_publish_light_state(slug, modules[i],
                                           cached ? cached : blk, cached);
                    sf_publish_light_extras(slug, modules[i],
                                            cached ? cached : blk);
                }
                cJSON_Delete(cached);
            }

            // Calibration tells the cloud about itself too, as a
            // synthetic reply — see mitm_proxy_report_calibration() for
            // why that shape rather than a mirrored command.
            cJSON *cal = cJSON_GetObjectItem(params, "calibration");
            if (cJSON_IsObject(cal)) {
                device_cache_merge(dev_mac, "calibration", cal);
                char *cal_json = cJSON_PrintUnformatted(cal);
                if (cal_json) {
                    mitm_proxy_report_calibration(dev_mac, cal_json);
                    cJSON_free(cal_json);
                }
            }
        }
        cJSON_free(json);
    }
    cJSON_Delete(cmd);
}

// Publishes every controller's current availability, retained.
//
// The per-controller topic is otherwise only written on a session change,
// so after a broker restart, a retained-message purge or a bridge reboot
// it could hold nothing at all -- and an entity whose availability topic
// is empty stays unavailable in Home Assistant indefinitely.
static void publish_all_availability(void)
{
    if (!s_client) return;
    device_registry_lock();
    char slugs[SB_MAX_DEVICES][DEV_SLUG_LEN];
    bool online[SB_MAX_DEVICES];
    int n = 0;
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        device_entry_t *d = device_registry_at(i);
        if (!d) continue;
        strncpy(slugs[n], d->slug, DEV_SLUG_LEN - 1);
        slugs[n][DEV_SLUG_LEN - 1] = '\0';
        online[n] = d->online;
        n++;
    }
    device_registry_unlock();
    for (int i = 0; i < n; i++) {
        ha_mqtt_publish_device_availability(slugs[i], online[i]);
    }
}

// Re-sends discovery and availability outside the MQTT event task.
//
// Discovery is paced with short delays to fit the client's outbox; doing
// that inside the event handler would stall the very task that has to
// drain the outbox. A flag stops a burst of birth messages from starting
// several of these at once.
static volatile bool s_rediscovery_running = false;

static void rediscovery_task(void *arg)
{
    (void)arg;
    // Home Assistant asks for a small random delay so a restart does not
    // meet every device's discovery at the same instant.
    vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 3000)));
    if (s_connected && ha_mqtt_ha_enabled()) {
        state_cache_clear();
        ha_mqtt_publish_discovery();
        publish_bridge_discovery();
        // After the burst, once the outbox has drained a little.
        vTaskDelay(pdMS_TO_TICKS(500));
        republish_availability();
        ha_mqtt_publish_bridge_state();
        ESP_LOGI(TAG, "Discovery re-sent for Home Assistant");
    }
    s_rediscovery_running = false;
    vTaskDelete(NULL);
}

static void schedule_rediscovery(void);

void ha_mqtt_rename_bridge(const char *name)
{
    prov_set_bridge_name(name);
    ESP_LOGI(TAG, "Bridge shown in Home Assistant as '%s'", prov_bridge_name());
    // Re-announcing with the same identifier and the new name renames the
    // existing device in Home Assistant; nothing has to be removed.
    if (s_connected && ha_mqtt_ha_enabled()) schedule_rediscovery();
}

void ha_mqtt_request_rediscovery(void)
{
    if (s_connected) schedule_rediscovery();
}

static void schedule_rediscovery(void)
{
    if (s_rediscovery_running) return;
    s_rediscovery_running = true;
    if (xTaskCreate(rediscovery_task, "sb_redisc", 6144, NULL, 4, NULL) != pdPASS) {
        s_rediscovery_running = false;
        ESP_LOGW(TAG, "Could not start the rediscovery task");
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "Connected to the MQTT broker");
            s_connected = true;
            s_connect_count++;
            s_connected_since_us = esp_timer_get_time();
            s_last_error[0] = '\0';
            hist_add('c', 0, 0);
            // A new session may be a broker that lost its retained store:
            // every state value is sent once again.
            state_cache_clear();

            // One subscription covering every device, so a controller
            // that is learned later needs no extra subscribe. Unknown
            // slugs are rejected in handle_command().
            esp_mqtt_client_subscribe(s_client, "spiderfarmer/+/command/#", 1);
            ESP_LOGI(TAG, "Subscribed to spiderfarmer/+/command/#");

            // The bridge is up: overwrite its own last will. Retained, so
            // Home Assistant sees it no matter when it subscribes.
            esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/availability",
                                    "online", 0, 1, 1);

            // Home Assistant's birth message. When it restarts it forgets
            // every entity until discovery arrives again; answering the
            // birth message re-sends it, which is what Home Assistant
            // recommends over relying on retained configs alone.
            esp_mqtt_client_subscribe(s_client, "homeassistant/status", 1);

            if (ha_mqtt_ha_enabled()) {
                // Clear the availability topic of the configured
                // device_id if no device actually uses that name.
                //
                // Earlier firmware published under a single fixed id.
                // Those retained topics survive on the broker and leave
                // Home Assistant showing a permanently unavailable
                // device that no longer exists.
                if (s_device_id[0]) {
                    device_registry_lock();
                    bool in_use = device_registry_find_by_slug(s_device_id) != NULL;
                    device_registry_unlock();
                    if (!in_use) {
                        char stale[160];
                        snprintf(stale, sizeof(stale),
                                 "spiderfarmer/%s/availability", s_device_id);
                        esp_mqtt_client_publish(s_client, stale, "", 0, 0, 1);
                        ESP_LOGI(TAG, "Cleared the stale '%s' availability topic",
                                 s_device_id);
                    }
                }

                // Orphan legacy device pages first, so a historical name
                // cannot clash with the live table that follows.
                clear_legacy_discovery();

                ha_mqtt_publish_discovery();
                publish_bridge_discovery();
                publish_all_availability();
                // State follows from the refresh task, which runs
                // outside the event handler. Publishing it here would
                // block the MQTT client task on its own pacing delays.
            }
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from the MQTT broker");
            if (s_connected) s_disconnect_count++;
            s_connected = false;
            s_connected_since_us = 0;
            // code 0: straight socket death. Carring a distinct marker
            // here because the ERROR event for the same drop can be
            // either before or after this one.
            hist_add('d', 0, 0);
            break;

        case MQTT_EVENT_ERROR: {
            // Record what actually went wrong. "Not connected" alone
            // sends people hunting for network faults when the real
            // cause is usually a rejected password or a wrong port.
            const char *what = "MQTT error";
            if (event->error_handle) {
                switch (event->error_handle->error_type) {
                    case MQTT_ERROR_TYPE_TCP_TRANSPORT: {
                        what = "Cannot reach the broker (check address, port "
                               "and that it is running)";
                        // The transport error alone narrows this down: a
                        // timeout, a RST and an unreachable host all read
                        // the same from "not connected", but need opposite
                        // fixes.
                        int sock_err = event->error_handle->esp_transport_sock_errno;
                        hist_add('e', MQTT_ERROR_TYPE_TCP_TRANSPORT,
                                 (uint16_t)sock_err);
                        ESP_LOGE(TAG, "%s (socket errno %d)", what, sock_err);
                        break;
                    }
                    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
                        switch (event->error_handle->connect_return_code) {
                            case MQTT_CONNECTION_REFUSE_PROTOCOL:
                                what = "Broker rejected the protocol version";
                                break;
                            case MQTT_CONNECTION_REFUSE_ID_REJECTED:
                                what = "Broker rejected the client id";
                                break;
                            case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
                                what = "Broker is unavailable";
                                break;
                            case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
                            case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
                                what = "Broker refused the username or password";
                                break;
                            default:
                                what = "Broker refused the connection";
                                break;
                        }
                        hist_add('e', MQTT_ERROR_TYPE_CONNECTION_REFUSED,
                                 (uint16_t)event->error_handle->connect_return_code);
                        break;
                    default:
                        hist_add('e', (uint16_t)event->error_handle->error_type, 0);
                        break;
                }
            } else {
                hist_add('e', 0, 0);
            }
            strncpy(s_last_error, what, sizeof(s_last_error) - 1);
            s_last_error[sizeof(s_last_error) - 1] = '\0';
            ESP_LOGE(TAG, "%s", what);
            break;
        }

        case MQTT_EVENT_DATA: {
            if (event->data_len <= 0) break;

            char topic[160] = {0};
            int tlen = event->topic_len < (int)sizeof(topic) - 1
                       ? event->topic_len : (int)sizeof(topic) - 1;
            memcpy(topic, event->topic, tlen);

            live_log_add_ha_in(topic, (const uint8_t *)event->data,
                               (size_t)event->data_len);

            // Home Assistant came (back) online. Its retained birth message
            // also arrives right after our own subscribe, which is fine:
            // the resend is idempotent.
            if (strcmp(topic, "homeassistant/status") == 0) {
                if (event->data_len == 6 &&
                    memcmp(event->data, "online", 6) == 0 &&
                    ha_mqtt_ha_enabled()) {
                    schedule_rediscovery();
                }
                break;
            }

            handle_command(topic, event->data, event->data_len);
            break;
        }

        default:
            break;
    }
}

// The CA certificate, kept alive for the lifetime of the client.
//
// esp-mqtt stores the pointer rather than copying the PEM, so it has to
// outlive ha_mqtt_start(). Allocated only when a certificate is actually
// configured, because 2 KB matters on this device.
static char *s_ca_cert = NULL;

void ha_mqtt_start(void)
{
    sb_prov_cfg_t *prov = malloc(sizeof(*prov));
    if (!prov) {
        ESP_LOGE(TAG, "Out of memory loading the MQTT configuration");
        return;
    }
    sb_prov_load(prov);

    if (prov->ha_mqtt_uri[0] == '\0') {
        ESP_LOGW(TAG, "No MQTT broker configured — staying off");
        strncpy(s_last_error, "No broker configured", sizeof(s_last_error) - 1);
        free(prov);
        return;
    }

    s_configured = true;
    strncpy(s_broker_uri, prov->ha_mqtt_uri, sizeof(s_broker_uri) - 1);
    strncpy(s_device_id, prov->device_id, sizeof(s_device_id) - 1);
    s_publish_mode = prov->publish_mode;
    snprintf(s_raw_topic, sizeof(s_raw_topic), "spiderfarmer/%s/raw", s_device_id);

    // The last will belongs to the bridge, not to a configured device id:
    // every entity -- the bridge's own and, combined with each controller's
    // availability, every controller entity -- names this topic, so the
    // broker marks all of them unavailable the moment the bridge vanishes.
    // It used to sit on spiderfarmer/<device_id>/availability, a topic no
    // entity referenced, so a dead bridge left everything showing online.
    static char lwt_topic[96];
    snprintf(lwt_topic, sizeof(lwt_topic), "spiderfarmer/bridge/availability");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = prov->ha_mqtt_uri,
        .credentials.username = prov->ha_mqtt_user[0] ? prov->ha_mqtt_user : NULL,
        .credentials.authentication.password = prov->ha_mqtt_pass[0] ? prov->ha_mqtt_pass : NULL,
        // A fixed client id. An unset id makes esp-mqtt send an empty
        // CONNECT client id, which every broker handles differently and
        // which loses any clustering-independent trace of which session
        // this is. A fixed id also keeps a reconnect from being seen by
        // the broker as a takeover of a different session.
        .credentials.client_id = "spiderbridge-esp32",
        .session.last_will.topic = lwt_topic,
        .session.last_will.msg = "offline",
        .session.last_will.retain = true,
        .session.last_will.qos = 1,
        .network.timeout_ms = 12000,
        // Discovery payloads run to several hundred bytes each and are
        // published in long runs. The default outbox is small enough
        // that a full republish overflows it, so give the client room
        // alongside the pacing in publish_discovery_for().
        .buffer.size = 2048,
        // The largest discovery payload is ~1.5 KB with its topic.
        .buffer.out_size = 2048,
        // 7168: measured only ~1.2 KB left at 6144 (status "stack_free")
        // within the first minute, with command translation and plan text
        // building running on this task.
        .task.stack_size = 7168,
        // Bounded: QoS-1 copies (availability, LWT) cannot grow without
        // limit while the broker is slow.
        .outbox.limit = 8192,
    };

    // --- TLS ---
    //
    // Only for an mqtts:// URI. Three cases, in order of preference:
    //
    //   CA certificate set  verify the broker against it. The only
    //                       option that actually proves identity.
    //   insecure ticked     encrypt but verify nothing. What most
    //                       self-signed home brokers need.
    //   neither             let esp-mqtt try its default trust store,
    //                       which will fail for a self-signed broker —
    //                       and the error says so on the status page.
    bool tls = strncmp(prov->ha_mqtt_uri, "mqtts://", 8) == 0 ||
               strncmp(prov->ha_mqtt_uri, "wss://", 6) == 0;

    if (tls && sb_prov_ca_cert_size() > 0) {
    // (TLS setup follows)
    // Loaded straight onto the heap: esp-mqtt keeps the pointer
    // rather than copying, so this has to outlive the call.
        free(s_ca_cert);
        s_ca_cert = malloc(2048);
        if (s_ca_cert && sb_prov_ca_cert_load(s_ca_cert, 2048) > 0) {
            cfg.broker.verification.certificate = s_ca_cert;
            cfg.broker.verification.certificate_len = strlen(s_ca_cert) + 1;
            ESP_LOGI(TAG, "TLS with the supplied CA certificate (%u bytes)",
                     (unsigned)strlen(s_ca_cert));
        } else {
            free(s_ca_cert);
            s_ca_cert = NULL;
            ESP_LOGE(TAG, "Could not load the CA certificate — "
                          "falling back to no verification");
            cfg.broker.verification.skip_cert_common_name_check = true;
            cfg.broker.verification.use_global_ca_store = false;
        }
    } else if (tls) {
        if (prov->mqtt_tls_insecure) {
            // Encrypted but unverified, by explicit choice.
            cfg.broker.verification.skip_cert_common_name_check = true;
            cfg.broker.verification.use_global_ca_store = false;
            ESP_LOGW(TAG, "TLS without certificate verification — "
                          "encrypted, but the broker's identity is not checked");
        } else {
            ESP_LOGW(TAG, "TLS with no CA certificate and verification left on; "
                          "a self-signed broker will be rejected");
        }
    }

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT client started, target: %s%s", prov->ha_mqtt_uri,
             tls ? " (TLS)" : "");
    ESP_LOGI(TAG, "Publish mode: %s",
             s_publish_mode == SB_PUB_BOTH ? "normalised + raw"
           : s_publish_mode == SB_PUB_RAW  ? "raw only"
                                           : "normalised only");

    // The client copied everything it needs except the certificate,
    // which is held in s_ca_cert.
    free(prov);

    // Keep the diagnostics current.
    //
    // Publishing them only on connect meant signal strength, free heap
    // and uptime froze at their startup values — worse than not having
    // them, because a stale number still looks like a reading.
    config_poll_add_hook(bridge_state_hook, 1);
}

void ha_mqtt_publish_raw(const uint8_t *payload, size_t len)
{
    if (!s_client || !ha_mqtt_raw_enabled()) return;
    esp_mqtt_client_publish(s_client, s_raw_topic, (const char *)payload,
                            (int)len, 0, 0);
}

// Retained, QoS 0.
//
// Retained means the broker keeps the last value and gives it to Home
// Assistant on connect, so nothing is lost by not using QoS 1 — and QoS
// 1 would hold a copy of every message in the client's outbox until it
// is acknowledged. With 14 state topics updating continuously next to
// two TLS sessions, that allocation is what exhausted the heap and
// stalled the web interface.
// --- Unchanged-value suppression ---
//
// Every controller frame republishes all of its ~100 state topics, so the
// broker and the live log saw ~30 identical messages a second. The values
// are retained, so an unchanged one carries no information; skipping it
// keeps the log readable (commands were scrolling out of it before the
// page could fetch them) and saves broker traffic.
//
// A small open-addressed table of (topic hash, value hash). Cleared on
// every new broker session and on a rediscovery, so a broker that lost its
// retained store is always refilled.
#define STATE_CACHE_SLOTS 256
static struct { uint32_t topic; uint32_t value; } s_state_cache[STATE_CACHE_SLOTS];
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1;   // 0 marks an empty slot
}

static void state_cache_clear(void)
{
    portENTER_CRITICAL(&s_state_mux);
    memset(s_state_cache, 0, sizeof(s_state_cache));
    portEXIT_CRITICAL(&s_state_mux);
}

// True when topic already holds value; otherwise records it and returns
// false. A full table simply stops suppressing, it never drops a publish.
static bool state_unchanged(const char *topic, const char *value)
{
    uint32_t th = fnv1a(topic), vh = fnv1a(value);
    bool same = false;
    portENTER_CRITICAL(&s_state_mux);
    for (int i = 0; i < STATE_CACHE_SLOTS; i++) {
        int k = (int)((th + (uint32_t)i) % STATE_CACHE_SLOTS);
        if (s_state_cache[k].topic == th) {
            same = (s_state_cache[k].value == vh);
            s_state_cache[k].value = vh;
            break;
        }
        if (s_state_cache[k].topic == 0) {
            s_state_cache[k].topic = th;
            s_state_cache[k].value = vh;
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_mux);
    return same;
}

void ha_mqtt_publish_state(const char *topic, const char *value)
{
    if (!s_client || !ha_mqtt_ha_enabled()) return;
    if (!s_connected) return;
    if (state_unchanged(topic, value)) return;
    esp_mqtt_client_publish(s_client, topic, value, 0, 0, 1);
    s_publish_count++;
    live_log_add_ha_out(topic, (const uint8_t *)value, strlen(value));
}

// Per-controller connection health.
//
// Published alongside the sensor values so an unstable link is visible
// in Home Assistant rather than only in the web interface. The counters
// reset on reboot by design — see device_registry.h.
void ha_mqtt_publish_connection(const char *mac, const char *slug)
{
    if (!s_client || !ha_mqtt_ha_enabled() || !slug || !slug[0]) return;

    uint32_t reconnects = 0, total = 0;
    char reason[DEV_REASON_LEN] = "";

    device_registry_lock();
    device_entry_t *d = device_registry_find(mac);
    if (d) {
        reconnects = d->reconnects;
        total = d->total_online_s;
        strncpy(reason, d->last_reason, sizeof(reason) - 1);
    }
    device_registry_unlock();

    uint32_t connected_for = device_registry_connected_for(mac);

    char topic[160], val[48];

    snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/connected_for", slug);
    snprintf(val, sizeof(val), "%u", (unsigned)connected_for);
    esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);

    snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/reconnects", slug);
    snprintf(val, sizeof(val), "%u", (unsigned)reconnects);
    esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);

    snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/total_online", slug);
    snprintf(val, sizeof(val), "%u", (unsigned)total);
    esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);

    snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/last_reason", slug);
    esp_mqtt_client_publish(s_client, topic,
                            reason[0] ? reason : "none", 0, 0, 1);

    // How long since the controller last said anything, and the cadence
    // it keeps. Together these distinguish a device that is idle from
    // one that has quietly gone away.
    uint32_t silence = 0, cadence = 0;
    mitm_proxy_liveness(mac, &silence, &cadence);

    snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/silence", slug);
    snprintf(val, sizeof(val), "%u", (unsigned)silence);
    esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);

    snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/cadence", slug);
    snprintf(val, sizeof(val), "%u", (unsigned)cadence);
    esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);
}

// Publishes the bridge's own state and its discovery entries.
//
// These appear in Home Assistant as a separate device, because they are
// settings of the bridge, not of any controller: the time zone applies
// to how every controller's schedule is interpreted.
void ha_mqtt_publish_bridge_state(void)
{
    if (!s_client || !ha_mqtt_ha_enabled()) return;

    time_status_t t;
    time_sync_get_status(&t);

    // The zone name, matching the controller topics.
    //
    // This used to carry the POSIX rules, which made the bridge and the
    // controllers disagree about what "timezone" meant. The rules are
    // published separately as tz_rules.
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/timezone",
                            prov_tz_name(),  0, 0, 1);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/tz_rules",
                            t.zone,  0, 0, 1);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/dst",
                            time_sync_dst_label(),  0, 0, 1);

    // The same setting as two switches.
    //
    // Under "Automatic" the switch shows whether summer time is in
    // force right now, so it always reflects the real clock rather than
    // going blank.
    int mode = prov_dst_mode();
    bool summer = (mode == SB_DST_SUMMER) ||
                  (mode == SB_DST_AUTO && t.is_dst);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/dst_on",
                            summer ? "ON" : "OFF", 0, 0, 1);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/dst_auto",
                            mode == SB_DST_AUTO ? "ON" : "OFF", 0, 0, 1);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/tz_push",
                            prov_tz_push() ? "ON" : "OFF", 0, 0, 1);

    // The network switches, reporting what is actually in force.
    {
        sb_prov_cfg_t *pc = malloc(sizeof(*pc));
        if (pc) {
            sb_prov_load(pc);
            const struct { const char *id; bool on; } NS[] = {
                { "wan",     pc->wan_open },
                { "cloudfw", pc->cloud_forward },
                { "dns_ex",  pc->allow_dns_offline },
                { "ntp_ex",  pc->allow_ntp_offline },
                { "dns_rd",  pc->dns_redirect },
                { "ntp_rd",  pc->ntp_redirect },
            };
            for (size_t i = 0; i < sizeof(NS) / sizeof(NS[0]); i++) {
                char tp[96];
                snprintf(tp, sizeof(tp), "spiderfarmer/bridge/state/%s",
                         NS[i].id);
                esp_mqtt_client_publish(s_client, tp,
                                        NS[i].on ? "ON" : "OFF", 0, 0, 1);
            }
            free(pc);
        }
    }
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/tz_name",
                            prov_tz_name(), 0, 0, 1);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/ntp_server",
                            t.server,  0, 0, 1);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/time",
                            t.now,  0, 0, 1);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/time_valid",
                            t.synced ? "ON" : "OFF",  0, 0, 1);

    // --- Diagnostics ---
    //
    // The same values the status page shows, so a problem can be seen
    // from Home Assistant without opening the web interface — which is
    // the thing most likely to be unreachable when something is wrong.
    //
    // Paced like discovery: this block publishes around fifteen messages
    // and sending them back to back fills the client's outbox, after
    // which the rest are dropped silently. That is how the clock topics
    // disappeared once the diagnostics were added below them.
    vTaskDelay(pdMS_TO_TICKS(40));

    wifi_status_t w;
    wifi_apsta_get_status(&w);

    char val[64];

    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/uplink",
                            w.sta_has_ip ? "ON" : "OFF", 0, 0, 1);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/ssid",
                            w.sta_ssid[0] ? w.sta_ssid : "none", 0, 0, 1);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/ip",
                            w.sta_ip[0] ? w.sta_ip : "none", 0, 0, 1);

    snprintf(val, sizeof(val), "%d", w.sta_rssi);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/rssi",
                            val, 0, 0, 1);

    snprintf(val, sizeof(val), "%d", w.sta_quality);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/signal",
                            val, 0, 0, 1);

    snprintf(val, sizeof(val), "%d", w.ap_clients);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/clients",
                            val, 0, 0, 1);

    vTaskDelay(pdMS_TO_TICKS(40));

    snprintf(val, sizeof(val), "%u", (unsigned)w.sta_disconnects);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/wifi_drops",
                            val, 0, 0, 1);

    snprintf(val, sizeof(val), "%u",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/heap",
                            val, 0, 0, 1);

    snprintf(val, sizeof(val), "%u", (unsigned)t.uptime_s);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/uptime",
                            val, 0, 0, 1);

    const esp_app_desc_t *app = esp_app_get_description();
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/version",
                            app->version, 0, 0, 1);

    vTaskDelay(pdMS_TO_TICKS(40));

    // One summary entity for the hotspot, with the client list as
    // attributes. Per-client entities would accumulate permanently every
    // time a phone connects once.
    char *attrs = malloc(512);
    if (attrs) {
        int o = snprintf(attrs, 512, "{\"clients\":[");
        int listed = 0;
        for (int i = 0; i < SB_MAX_DEVICES && o < 460; i++) {
            device_registry_lock();
            device_entry_t *d = device_registry_at(i);
            char nm[DEV_NAME_LEN] = "", mc[DEV_MAC_LEN] = "";
            bool on = false;
            if (d) {
                strncpy(nm, d->name, sizeof(nm) - 1);
                strncpy(mc, d->mac, sizeof(mc) - 1);
                on = d->online;
            }
            device_registry_unlock();
            if (!mc[0]) continue;

            o += snprintf(attrs + o, 512 - o,
                          "%s{\"name\":\"%s\",\"mac\":\"%s\",\"online\":%s}",
                          listed ? "," : "", nm, mc, on ? "true" : "false");
            listed++;
        }
        snprintf(attrs + o, 512 - o, "],\"hotspot\":\"%s\"}",
                 w.ap_ssid[0] ? w.ap_ssid : "");
        esp_mqtt_client_publish(s_client,
                                "spiderfarmer/bridge/state/clients_attr",
                                attrs, 0, 0, 1);
        free(attrs);
    }
}

// Discovery for the bridge's own entities.
static void publish_bridge_discovery(void)
{
    if (!s_client || !ha_mqtt_ha_enabled()) return;

    // Device, origin and availability shared by every bridge entity. The
    // availability topic is the bridge's own last will, so these go
    // unavailable together when the bridge drops off the broker.
    // The device name is the one set under Settings; the identifier stays
    // fixed, so a rename relabels the existing device in Home Assistant.
    char DEV[256];
    snprintf(DEV, sizeof(DEV),
        "\"dev\":{\"ids\":[\"spiderbridge\"],\"name\":\"%s\","
        "\"mf\":\"SpiderBridge\",\"mdl\":\"ESP32 Bridge\"},"
        "\"o\":{\"name\":\"SpiderBridge\"},"
        "\"avty_t\":\"spiderfarmer/bridge/availability\"",
        prov_bridge_name());

    // Sized for the largest payload here, the time zone dropdown with
    // its 32 options.
    char topic[160], payload[1024];

    // Time zone, as a dropdown of the common ones.
    //
    // A select rather than free text because a zone name has to match
    // exactly — a typo would be accepted and then silently ignored.
    // The list is deliberately short: the firmware knows 120 zones, but
    // a Home Assistant dropdown with 120 entries is unusable, and the
    // text entity below covers anything missing.
    snprintf(topic, sizeof(topic),
             "homeassistant/select/spiderbridge/tz_pick/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Time zone\",\"uniq_id\":\"sb_tz_pick\","
             "\"stat_t\":\"spiderfarmer/bridge/state/tz_name\","
             "\"cmd_t\":\"spiderfarmer/bridge/command/timezone/set\","
             "\"options\":[\"Europe/Berlin\",\"Europe/London\","
             "\"Europe/Paris\",\"Europe/Madrid\",\"Europe/Rome\","
             "\"Europe/Amsterdam\",\"Europe/Vienna\",\"Europe/Zurich\","
             "\"Europe/Warsaw\",\"Europe/Stockholm\",\"Europe/Helsinki\","
             "\"Europe/Athens\",\"Europe/Lisbon\",\"Europe/Dublin\","
             "\"Europe/Moscow\",\"America/New_York\",\"America/Chicago\","
             "\"America/Denver\",\"America/Los_Angeles\","
             "\"America/Phoenix\",\"America/Toronto\","
             "\"America/Vancouver\",\"America/Sao_Paulo\","
             "\"Asia/Tokyo\",\"Asia/Shanghai\",\"Asia/Singapore\","
             "\"Asia/Kolkata\",\"Asia/Dubai\",\"Australia/Sydney\","
             "\"Australia/Perth\",\"Pacific/Auckland\",\"UTC\"],"
             "\"ic\":\"mdi:earth\",%s}", DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    // Free text for any zone not in the shortlist. Same command topic,
    // so the two stay in step; an unknown name is rejected with a log
    // line rather than silently applied.
    snprintf(topic, sizeof(topic),
             "homeassistant/text/spiderbridge/timezone/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Time zone (type any)\",\"uniq_id\":\"sb_timezone\","
             "\"stat_t\":\"spiderfarmer/bridge/state/tz_name\","
             "\"cmd_t\":\"spiderfarmer/bridge/command/timezone/set\","
             "\"max\":40,\"ic\":\"mdi:earth-plus\",\"ent_cat\":\"config\",%s}",
             DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/select/spiderbridge/dst/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Daylight saving\",\"uniq_id\":\"sb_dst\","
             "\"stat_t\":\"spiderfarmer/bridge/state/dst\","
             "\"cmd_t\":\"spiderfarmer/bridge/command/dst/set\","
             "\"options\":[\"Automatic\",\"Standard\",\"Summer\"],"
             "\"ic\":\"mdi:sun-clock\",%s}", DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    // The same choice as a plain switch.
    //
    // The select above is complete but awkward in a dashboard; most of
    // the time the question is simply "summer time on or off". Both act
    // on the same setting and both report the same state, so flipping
    // one updates the other.
    //
    // While "Automatic" is in force the switch reflects whatever the
    // rules currently say, and turning it pins the clock — which is the
    // honest behaviour: the switch always does something.
    snprintf(topic, sizeof(topic),
             "homeassistant/switch/spiderbridge/dst_on/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Summer time\",\"uniq_id\":\"sb_dst_on\","
             "\"stat_t\":\"spiderfarmer/bridge/state/dst_on\","
             "\"cmd_t\":\"spiderfarmer/bridge/command/dst_on/set\","
             "\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
             "\"ic\":\"mdi:weather-sunny\",%s}", DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/switch/spiderbridge/dst_auto/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Follow the zone rules\",\"uniq_id\":\"sb_dst_auto\","
             "\"stat_t\":\"spiderfarmer/bridge/state/dst_auto\","
             "\"cmd_t\":\"spiderfarmer/bridge/command/dst_auto/set\","
             "\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
             "\"ic\":\"mdi:calendar-sync\",%s}", DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    // --- The network settings, as switches ---
    //
    // The same options as the settings page. Having them here means a
    // change can be made from Home Assistant without opening the web
    // interface — which is the thing least likely to be reachable when
    // one of these is set wrongly.
    //
    // Each one applies immediately and takes effect without a reboot.
    static const struct {
        const char *id;
        const char *name;
        const char *icon;
    } NET_SW[] = {
        { "tz_push",  "Apply clock to all controllers", "mdi:sync" },
        { "wan",      "Internet for controllers",       "mdi:web" },
        { "cloudfw",  "Mirror to the Spider Farmer cloud", "mdi:cloud-upload" },
        { "dns_ex",   "Name resolution while offline",  "mdi:dns" },
        { "ntp_ex",   "Time sync while offline",        "mdi:clock-check" },
        { "dns_rd",   "Redirect name lookups",          "mdi:directions-fork" },
        { "ntp_rd",   "Redirect time requests",         "mdi:clock-fast" },
    };

    for (size_t i = 0; i < sizeof(NET_SW) / sizeof(NET_SW[0]); i++) {
        if ((i % 4) == 3) vTaskDelay(pdMS_TO_TICKS(30));
        snprintf(topic, sizeof(topic),
                 "homeassistant/switch/spiderbridge/%s/config", NET_SW[i].id);
        snprintf(payload, sizeof(payload),
                 "{\"name\":\"%s\",\"uniq_id\":\"sb_%s\","
                 "\"stat_t\":\"spiderfarmer/bridge/state/%s\","
                 "\"cmd_t\":\"spiderfarmer/bridge/command/%s/set\","
                 "\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
                 "\"ic\":\"%s\",\"ent_cat\":\"config\",%s}",
                 NET_SW[i].name, NET_SW[i].id, NET_SW[i].id, NET_SW[i].id,
                 NET_SW[i].icon, DEV);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
    }

    // The bridge's own name in Home Assistant, editable from there too.
    snprintf(topic, sizeof(topic),
             "homeassistant/text/spiderbridge/bridge_name/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Device name\",\"uniq_id\":\"sb_bridge_name\","
             "\"stat_t\":\"spiderfarmer/bridge/state/bridge_name\","
             "\"cmd_t\":\"spiderfarmer/bridge/command/bridge_name/set\","
             "\"max\":32,\"ic\":\"mdi:rename\",\"ent_cat\":\"config\",%s}", DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/bridge_name",
                            prov_bridge_name(), 0, 0, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/text/spiderbridge/ntp/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"NTP server\",\"uniq_id\":\"sb_ntp\","
             "\"stat_t\":\"spiderfarmer/bridge/state/ntp_server\","
             "\"cmd_t\":\"spiderfarmer/bridge/command/ntp_server/set\","
             "\"max\":63,\"ic\":\"mdi:clock-check\",%s}", DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/spiderbridge/time/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Bridge time\",\"uniq_id\":\"sb_time\","
             "\"stat_t\":\"spiderfarmer/bridge/state/time\","
             "\"ic\":\"mdi:clock-outline\",%s}", DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/spiderbridge/time_valid/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Clock synchronised\",\"uniq_id\":\"sb_time_valid\","
             "\"stat_t\":\"spiderfarmer/bridge/state/time_valid\","
             "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"dev_cla\":\"connectivity\",%s}",
             DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    // --- Diagnostics ---
    //
    // Paced like the per-device discovery: publishing this many retained
    // messages back to back exhausts the client's outbox, which took the
    // web interface down once before the pacing was added.
    static const struct {
        const char *id;     // entity id suffix and state topic suffix
        const char *name;
        const char *extra;  // device class, unit, icon
    } DIAG[] = {
        { "ssid",       "Uplink network",   "\"ic\":\"mdi:wifi\"" },
        { "ip",         "Uplink address",   "\"ic\":\"mdi:ip-network\"" },
        { "rssi",       "Uplink signal",
          "\"dev_cla\":\"signal_strength\",\"unit_of_meas\":\"dBm\","
          "\"stat_cla\":\"measurement\"" },
        { "signal",     "Uplink quality",
          "\"unit_of_meas\":\"%\",\"stat_cla\":\"measurement\","
          "\"ic\":\"mdi:wifi-strength-3\"" },
        { "clients",    "Hotspot clients",
          "\"stat_cla\":\"measurement\",\"ic\":\"mdi:account-multiple\","
          "\"json_attr_t\":\"spiderfarmer/bridge/state/clients_attr\"" },
        { "wifi_drops", "Uplink drops",
          "\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:wifi-off\"" },
        { "heap",       "Free memory",
          "\"unit_of_meas\":\"kB\",\"stat_cla\":\"measurement\","
          "\"ic\":\"mdi:memory\"" },
        { "uptime",     "Uptime",
          "\"dev_cla\":\"duration\",\"unit_of_meas\":\"s\","
          "\"stat_cla\":\"total_increasing\"" },
        { "version",    "Firmware version", "\"ic\":\"mdi:chip\"" },
    };

    for (size_t i = 0; i < sizeof(DIAG) / sizeof(DIAG[0]); i++) {
        if ((i % 4) == 3) vTaskDelay(pdMS_TO_TICKS(30));
        snprintf(topic, sizeof(topic),
                 "homeassistant/sensor/spiderbridge/%s/config", DIAG[i].id);
        snprintf(payload, sizeof(payload),
                 "{\"name\":\"%s\",\"uniq_id\":\"sb_%s\","
                 "\"stat_t\":\"spiderfarmer/bridge/state/%s\","
                 "\"ent_cat\":\"diagnostic\",%s,%s}",
                 DIAG[i].name, DIAG[i].id, DIAG[i].id, DIAG[i].extra, DEV);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
    }

    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/spiderbridge/uplink/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Uplink connected\",\"uniq_id\":\"sb_uplink\","
             "\"stat_t\":\"spiderfarmer/bridge/state/uplink\","
             "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"dev_cla\":\"connectivity\","
             "\"ent_cat\":\"diagnostic\",%s}", DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    vTaskDelay(pdMS_TO_TICKS(30));

    // --- Restart and factory reset ---
    //
    // Restart is a plain button. Factory reset is a text entity that only
    // acts on exactly "RESET", so no single click can discard the WiFi
    // credentials and every device name.
    snprintf(topic, sizeof(topic),
             "homeassistant/button/spiderbridge/reboot/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Restart bridge\",\"uniq_id\":\"sb_reboot\","
             "\"cmd_t\":\"spiderfarmer/bridge/command/reboot/set\","
             "\"pl_prs\":\"PRESS\",\"dev_cla\":\"restart\","
             "\"ent_cat\":\"config\",%s}", DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/text/spiderbridge/factory_reset/config");
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Factory reset (type RESET)\","
             "\"uniq_id\":\"sb_factory_reset\","
             "\"stat_t\":\"spiderfarmer/bridge/state/reset_hint\","
             "\"cmd_t\":\"spiderfarmer/bridge/command/factory_reset/set\","
             "\"max\":16,\"ic\":\"mdi:nuke\",\"ent_cat\":\"config\",%s}", DEV);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    esp_mqtt_client_publish(s_client, "spiderfarmer/bridge/state/reset_hint",
                            "type RESET", 0, 0, 1);

    ESP_LOGI(TAG, "Bridge entities published (clock, diagnostics, controls)");
}

void ha_mqtt_get_status(ha_mqtt_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->configured   = s_configured;
    out->connected    = s_connected;
    out->connects     = s_connect_count;
    out->disconnects  = s_disconnect_count;
    out->publishes    = s_publish_count;
    strncpy(out->broker, s_broker_uri, sizeof(out->broker) - 1);
    strncpy(out->last_error, s_last_error, sizeof(out->last_error) - 1);

    out->mode = s_publish_mode;
    if (s_connected_since_us) {
        out->uptime_s = (uint32_t)((esp_timer_get_time() - s_connected_since_us)
                                   / 1000000);
    }
}

void ha_mqtt_publish_availability(bool online)
{
    if (!s_client) return;
    char topic[96];
    snprintf(topic, sizeof(topic), "spiderfarmer/%s/availability", s_device_id);
    esp_mqtt_client_publish(s_client, topic, online ? "online" : "offline", 0, 0, 1);
}

// ---------------------------------------------------------------------------
// Discovery
//
// Payloads live in ha_discovery_table.c, generated from the discovery
// messages the original Python bridge published. Keeping them
// byte-identical means Home Assistant sees the same device with the same
// entity IDs, so existing history and automations keep working.
//
// Two placeholders are expanded at runtime:
//     $D -> device id    (e.g. "ggs_1")
//     $N -> display name (e.g. "GGS")
// ---------------------------------------------------------------------------
// $D becomes the device's slug and $N its display name, so renaming a
// device in the web interface changes both its topics and the labels
// Home Assistant shows, without touching anything on the cloud side.
// $B is the bridge's own display name, set under Settings.
static int expand_placeholders_for(const char *src, const char *slug,
                                   const char *disp_name,
                                   char *dst, size_t dst_sz)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0'; si++) {
        if (src[si] == '$' && (src[si + 1] == 'D' || src[si + 1] == 'N' ||
                               src[si + 1] == 'B')) {
            const char *rep = (src[si + 1] == 'D') ? slug
                            : (src[si + 1] == 'N') ? disp_name
                            : prov_bridge_name();
            size_t rl = strlen(rep);
            if (di + rl >= dst_sz) return -1;
            memcpy(dst + di, rep, rl);
            di += rl;
            si++;
            continue;
        }
        if (di + 1 >= dst_sz) return -1;
        dst[di++] = src[si];
    }
    dst[di] = '\0';
    return (int)di;
}

// The wildcard subscription already covers every device, so this is a
// no-op kept for call-site clarity where a device becomes known.
static void subscribe_device(const char *slug)
{
    (void)slug;
}

// Discovery topics that earlier firmware published and this one no
// longer does.
//
// Several settings were announced once per fan mode — three "Natural
// Wind" switches, three "Speed" numbers and so on — all bound to the
// same state and command topic. On the wire each of those is a single
// field, so the copies could only ever disagree with each other. They
// are now published once, under the main device.
//
// The old payloads are retained on the broker, so without clearing them
// explicitly Home Assistant keeps the superseded entities forever. An
// empty retained payload is what removes them.
//
// The list itself is generated into ha_discovery_stale.c, as this table
// minus a snapshot of what earlier firmware published. It was hand-written
// before that, and a hand-written list goes stale exactly when an entity is
// renamed — which is when it is needed.
//
// Why the list exists at all, and why it has to cover the per-mode pseudo
// devices ("Cycle settings", "Schedule settings", "PPFD settings") that
// older tables announced: those were each given their own device block --
// "GGS Fan Exhaust Cycle Mode" and eight siblings -- with the component
// type in the unique_id, spiderfarmer_<slug>_fan_schedule_speed_cycle.
// Their entities have since been renamed to drop the mode suffix and
// republished under the main controller, so the old ids are not merely
// superseded but orphaned: nothing republishes them, the rename path
// never clears them, and because the payloads are retained on the broker
// Home Assistant keeps the whole device pages alive indefinitely. That is
// why the device list grew to ten entries when it should hold exactly two:
// SpiderBridge and the controller.
//
// An empty retained payload is what removes a discovery config.
#define STALE_DISCOVERY_TOPICS HA_STALE_DISCOVERY_TOPICS
#define STALE_DISCOVERY_COUNT  HA_STALE_DISCOVERY_COUNT

static void clear_stale_discovery_for(const char *slug)
{
    if (!s_client || !slug || !slug[0]) return;

    char topic[256];
    for (size_t i = 0; i < STALE_DISCOVERY_COUNT; i++) {
        if (expand_placeholders_for(STALE_DISCOVERY_TOPICS[i], slug, slug,
                                    topic, sizeof(topic)) < 0) {
            continue;
        }
        esp_mqtt_client_publish(s_client, topic, "", 0, 0, 1);
        if ((i % 8) == 7) vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "Cleared %u superseded discovery topics for '%s'",
             (unsigned)STALE_DISCOVERY_COUNT, slug);
}
// Entities that describe the bridge rather than one controller.
//
// The heap and version sensors are the same for every controller -- there is
// only one ESP32 on this network -- so they must not be republished per
// device. Repeating them under each controller's identifiers created one
// duplicate pair of entities per controller, all fighting over the single
// set of values. They are published once, under a slug of their own, and
// only that one publication carries the bridge device.
//
// The WAN switch is deliberately NOT here. It gates the controllers'
// internet access rather than anything on the bridge, so it stays with the
// controller that switches it.
static const ha_discovery_entry_t *bridge_entry(const ha_discovery_entry_t *e)
{
    return strstr(e->topic, "_bridge_") ? e : NULL;
}

static int publish_discovery_subset(const char *slug, const char *disp_name,
                                    bool bridge_only)
{
    if (!s_client || !slug || !slug[0]) return -1;

    const size_t topic_sz = 256;
    const size_t payload_sz = HA_DISCOVERY_MAX_PAYLOAD + 256;

    char *topic = malloc(topic_sz);
    char *payload = malloc(payload_sz);
    if (!topic || !payload) {
        ESP_LOGE(TAG, "Out of memory while publishing discovery");
        free(topic);
        free(payload);

        return -1;
    }

    size_t published = 0, failed = 0;
    for (size_t i = 0; i < HA_DISCOVERY_COUNT; i++) {
        const ha_discovery_entry_t *e = &HA_DISCOVERY_TABLE[i];
        const bool is_bridge = bridge_entry(e) != NULL;
        if (is_bridge != bridge_only) continue;

        if (expand_placeholders_for(e->topic, slug, disp_name, topic, topic_sz) < 0 ||
            expand_placeholders_for(e->payload, slug, disp_name, payload, payload_sz) < 0) {
            failed++;
            continue;
        }

        // QoS 0, deliberately.
        //
        // A QoS 1 message is copied into the client's outbox and kept
        // there until the broker acknowledges it. Publishing this table
        // allocates one such copy per entity, and with two TLS sessions
        // already holding most of the internal heap that allocation
        // fails: observed as a flood of "outbox_enqueue: Memory
        // exhausted" and only 24 of 72 entities reaching Home Assistant.
        //
        // These messages are retained, so the broker stores them and
        // replays them to Home Assistant on every connect. The delivery
        // guarantee QoS 1 adds is redundant here, while its memory cost
        // is what broke the publish.
        if ((i % 8) == 7) vTaskDelay(pdMS_TO_TICKS(20));

        if (esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1) < 0) {
            failed++;
        } else {
            published++;
        }
    }

    if (!bridge_only && ha_mqtt_publish_plan_template_select(slug)) published++;

    free(topic);
    free(payload);

    if (failed) {
        ESP_LOGW(TAG, "Discovery for '%s': %u published, %u failed",
                 disp_name, (unsigned)published, (unsigned)failed);
    } else {
        ESP_LOGI(TAG, "Discovery for '%s': %u entities", disp_name,
                 (unsigned)published);
    }

    return published ? 0 : (failed ? -1 : 0);
}

// Publishes the whole discovery table for one device: the controller's own
// entities, plus the bridge entities on their first publication. The
// bridge subset is published again for every subsequent controller, but it
// lands on the same two topics, so a repeat is a cheap overwrite rather
// than a second set of entities.
static void publish_discovery_for(const char *slug, const char *disp_name)
{
    if (!s_client || !slug || !slug[0]) return;

    publish_discovery_subset(slug, disp_name, false);

    // After the current table, so a topic that appears in both lists
    // ends up with its current payload rather than being cleared.
    clear_stale_discovery_for(slug);

    publish_discovery_subset("bridge", prov_bridge_name(), true);
}

// Removes the retained discovery topics of one slug.
//
// Used both when forgetting a device and as the first half of a rename:
// the old identifiers have to be cleared explicitly, otherwise Home
// Assistant keeps the previous entities forever as stale duplicates.
static void clear_discovery_for(const char *slug)
{
    if (!s_client || !slug || !slug[0]) return;

    char *topic = malloc(256);
    if (!topic) return;

    size_t removed = 0;
    for (size_t i = 0; i < HA_DISCOVERY_COUNT; i++) {
        if (expand_placeholders_for(HA_DISCOVERY_TABLE[i].topic, slug,
                                    slug, topic, 256) < 0) {
            continue;
        }
        // An empty retained payload tells Home Assistant to drop the
        // entity, rather than leaving it stuck as "unavailable".
        esp_mqtt_client_publish(s_client, topic, "", 0, 0, 1);
        removed++;
    }

    // The availability topic is retained too and would otherwise survive
    // its device.
    char avail[160];
    snprintf(avail, sizeof(avail), "spiderfarmer/%s/availability", slug);
    esp_mqtt_client_publish(s_client, avail, "", 0, 0, 1);

    free(topic);
    ESP_LOGI(TAG, "Cleared %u retained topics for '%s'", (unsigned)removed, slug);
}

// Legacy slug names that older firmware published discovery under before
// the controllers were renamed in the interface.
//
// Every retained config beneath one of these still holds a device in Home
// Assistant: the topics use the slug, so renaming the device in the bridge
// moved the live entities to a new topic and left the old ones orphaned.
// The stale-topic list alone cannot reach them, because its $D placeholder
// expands to the slug a device has NOW -- the old topics sit under the
// slug those devices had THEN.
//
// The list is deliberately short and closed. A slug the user renamed
// yesterday is already handled by the rename path (it takes the old slug
// with it); these cover the names firmware itself assigned before the
// rename existed at all.
static const char *const LEGACY_SLUGS[] = {
    "ggs_1",          // the factory "device_id" used by all early firmware
    "zelt_rechts",    // the second controller of the same era
};
#define LEGACY_SLUG_COUNT \
    (sizeof(LEGACY_SLUGS) / sizeof(LEGACY_SLUGS[0]))

// Removes the retained discovery of legacy names once per connection,
// before the current table, so every orphan device page created by
// earlier firmware disappears. Runs under the same pacing as the table.
static void clear_legacy_discovery(void)
{
    if (!s_client || !ha_mqtt_ha_enabled()) return;

    for (size_t s = 0; s < LEGACY_SLUG_COUNT; s++) {
        clear_discovery_for(LEGACY_SLUGS[s]);
        clear_stale_discovery_for(LEGACY_SLUGS[s]);
    }
}

void ha_mqtt_announce_device(const char *mac)
{
    if (!s_client || !ha_mqtt_ha_enabled()) return;

    device_registry_lock();
    device_entry_t *d = device_registry_find(mac);
    if (!d) {
        device_registry_unlock();
        return;
    }
    char slug[DEV_SLUG_LEN], name[DEV_NAME_LEN];
    strncpy(slug, d->slug, sizeof(slug) - 1); slug[sizeof(slug) - 1] = '\0';
    strncpy(name, d->name, sizeof(name) - 1); name[sizeof(name) - 1] = '\0';
    bool need_discovery = !d->discovery_sent;
    d->discovery_sent = true;
    device_registry_unlock();

    if (need_discovery) {
        publish_discovery_for(slug, name);
        subscribe_device(slug);
    }
    ha_mqtt_publish_device_availability(slug, true);
}

// Republishing roughly 150 retained messages takes seconds once it is
// paced to fit the outbox. Doing that inside the HTTP handler holds the
// socket open until the browser gives up, so it runs on its own task and
// the request returns immediately.
typedef struct {
    char mac[DEV_MAC_LEN];
    char old_slug[DEV_SLUG_LEN];
} rename_job_t;

static void rename_task(void *arg)
{
    rename_job_t *job = (rename_job_t *)arg;

    if (job->old_slug[0]) clear_discovery_for(job->old_slug);

    device_registry_lock();
    device_entry_t *d = device_registry_find(job->mac);
    if (d) d->discovery_sent = false;
    device_registry_unlock();

    ha_mqtt_announce_device(job->mac);

    free(job);
    vTaskDelete(NULL);
}

void ha_mqtt_rename_device(const char *mac, const char *old_slug)
{
    if (!s_client || !mac) return;

    rename_job_t *job = calloc(1, sizeof(*job));
    if (!job) return;

    strncpy(job->mac, mac, sizeof(job->mac) - 1);
    if (old_slug) strncpy(job->old_slug, old_slug, sizeof(job->old_slug) - 1);

    // Priority 3, below the HTTP server: republishing is background work
    // and must not make the interface unresponsive while it runs.
    if (xTaskCreate(rename_task, "sb_rename", 6144, job, 3, NULL) != pdPASS) {
        free(job);
        ESP_LOGW(TAG, "Could not start the rename task");
    }
}

void ha_mqtt_forget_device(const char *slug)
{
    clear_discovery_for(slug);
}

void ha_mqtt_publish_device_availability(const char *slug, bool online)
{
    if (!s_client || !slug || !slug[0]) return;
    char topic[160];
    snprintf(topic, sizeof(topic), "spiderfarmer/%s/availability", slug);
    if (esp_mqtt_client_publish(s_client, topic, online ? "online" : "offline",
                                0, 0, 1) < 0)
        s_avail_retry = true;
}

void ha_mqtt_publish_device_calibration(const char *mac, const char *slug)
{
    if (!s_client || !slug || !slug[0] || !mac) return;

    float temp = 0, humi = 0, co2 = 0, ppfd = 0;
    device_registry_get_cal(mac, &temp, &humi, &co2, &ppfd);

    const struct { const char *suffix; float v; } cal[] = {
        { "cal_temp", temp }, { "cal_humi", humi },
        { "cal_co2",  co2  }, { "cal_ppfd", ppfd },
    };
    for (size_t i = 0; i < sizeof(cal) / sizeof(cal[0]); i++) {
        char topic[160], val[16];
        snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/%s",
                 slug, cal[i].suffix);
        snprintf(val, sizeof(val), "%.1f", cal[i].v);
        esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);
    }
}

// Publishes discovery for every known device, used on broker connect.
void ha_mqtt_publish_discovery(void)
{
    device_registry_lock();
    char slugs[SB_MAX_DEVICES][DEV_SLUG_LEN];
    char names[SB_MAX_DEVICES][DEV_NAME_LEN];
    int n = 0;
    for (int i = 0; i < SB_MAX_DEVICES && n < SB_MAX_DEVICES; i++) {
        device_entry_t *d = device_registry_at(i);
        if (!d) continue;
        strncpy(slugs[n], d->slug, DEV_SLUG_LEN - 1); slugs[n][DEV_SLUG_LEN - 1] = '\0';
        strncpy(names[n], d->name, DEV_NAME_LEN - 1); names[n][DEV_NAME_LEN - 1] = '\0';
        d->discovery_sent = true;
        n++;
    }
    device_registry_unlock();

    for (int i = 0; i < n; i++) {
        publish_discovery_for(slugs[i], names[i]);
        subscribe_device(slugs[i]);
    }
    if (n == 0) {
        ESP_LOGI(TAG, "No devices known yet — discovery follows on first contact");
    }
}

void ha_mqtt_clear_discovery(void)
{
    device_registry_lock();
    char slugs[SB_MAX_DEVICES][DEV_SLUG_LEN];
    int n = 0;
    for (int i = 0; i < SB_MAX_DEVICES && n < SB_MAX_DEVICES; i++) {
        device_entry_t *d = device_registry_at(i);
        if (!d) continue;
        strncpy(slugs[n], d->slug, DEV_SLUG_LEN - 1); slugs[n][DEV_SLUG_LEN - 1] = '\0';
        d->discovery_sent = false;
        n++;
    }
    device_registry_unlock();

    for (int i = 0; i < n; i++) clear_discovery_for(slugs[i]);
}
