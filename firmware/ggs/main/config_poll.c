#include <string.h>
#include "device_cache.h"
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "sb_config.h"
#include "mitm_proxy.h"
#include "config_poll.h"

static const char *TAG = "config_poll";

static char s_mac[16] = "";
static char s_uid[64] = "";
static bool s_running = false;

// The modules whose stored configuration Home Assistant needs.
//
// "target" and "calibration" are addressed with a one-element keyPath and no
// "device" prefix, the same shape: neither is part of any device block, so
// they are requested on their own rather than under a device key. The day
// cycle settings in particular are a flat object of their own.
static const struct {
    const char *name;
    bool        flat;   // keyPath ["name"] instead of ["device", name]
} POLL_MODULES[] = {
    { "light",       false },
    { "light2",      false },
    { "fan",         false },
    { "blower",      false },
    { "target",      true  },
    { "calibration", true  },
    { "alarm",       true  },
    { "plan",        true  },
};
#define POLL_MODULE_COUNT (sizeof(POLL_MODULES) / sizeof(POLL_MODULES[0]))

void config_poll_set_session(const char *mac, const char *uid)
{
    if (mac) {
        strncpy(s_mac, mac, sizeof(s_mac) - 1);
        s_mac[sizeof(s_mac) - 1] = '\0';
    } else {
        s_mac[0] = '\0';
    }
    if (uid) {
        strncpy(s_uid, uid, sizeof(s_uid) - 1);
        s_uid[sizeof(s_uid) - 1] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Diagnostics
//
// The poll used to be switched off entirely, because a test that fired four
// getConfigField requests half a second apart was followed by a hang severe
// enough to need a power cycle. Whether that burst actually caused it was
// never established.
//
// These counters exist so the next attempt is readable rather than anecdotal:
// if the device wedges again, the log says how far the round got and what
// the heap looked like at that moment, instead of just stopping mid-burst.
// ---------------------------------------------------------------------------

static volatile uint32_t s_round_seq = 0;
static volatile uint32_t s_sent_ok = 0;
static volatile uint32_t s_sent_fail = 0;
static volatile uint32_t s_heap_min = 0;
static uint32_t s_last_activity_ms = 0;
// Set for the length of a round and cleared at its end, so the watchdog
// only watches a round that is genuinely in progress.
static volatile bool s_round_active = false;

static void poll_note_activity(void)
{
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

// Marks the end of a round. The watchdog only reports a round that is
// still marked active, so without this the wait between rounds was
// reported as a stall.
static void poll_note_round_done(void)
{
    s_round_active = false;
}

static uint32_t heap_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

// ---------------------------------------------------------------------------
// Housekeeping hooks
//
// This task already wakes every second. Jobs that used to own a task of
// their own just to sleep (hourly time-zone push, 1 s bridge diagnostics)
// run from here instead, which saves their stacks -- 4 KB each.
// ---------------------------------------------------------------------------
#define MAX_HOOKS 4
static struct { config_poll_hook_t fn; uint32_t every_s; uint32_t left; } s_hooks[MAX_HOOKS];
static int s_hook_count = 0;

void config_poll_add_hook(config_poll_hook_t fn, uint32_t every_s)
{
    if (!fn || s_hook_count >= MAX_HOOKS) return;
    s_hooks[s_hook_count].fn = fn;
    s_hooks[s_hook_count].every_s = every_s ? every_s : 1;
    s_hooks[s_hook_count].left = every_s ? every_s : 1;
    s_hook_count++;
}

static void run_hooks(void)
{
    for (int i = 0; i < s_hook_count; i++) {
        if (--s_hooks[i].left == 0) {
            s_hooks[i].left = s_hooks[i].every_s;
            s_hooks[i].fn();
        }
    }
}

// ---------------------------------------------------------------------------
// Request sending
// ---------------------------------------------------------------------------

static bool send_request(const char *mac, const char *uid,
                         const char *module, bool flat)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "method", "getConfigField");
    cJSON_AddStringToObject(root, "pid", mac);

    cJSON *params = cJSON_CreateObject();
    cJSON *keypath = cJSON_CreateArray();
    if (flat) {
        // One element, no "device" prefix — the shape the vendor app uses
        // for calibration and for the day cycle target block. A two-element
        // path here is silently ignored by the controller, which is
        // indistinguishable from a dead link.
        cJSON_AddItemToArray(keypath, cJSON_CreateString(module));
    } else {
        cJSON_AddItemToArray(keypath, cJSON_CreateString("device"));
        cJSON_AddItemToArray(keypath, cJSON_CreateString(module));
    }
    cJSON_AddItemToObject(params, "keyPath", keypath);
    cJSON_AddItemToObject(root, "params", params);

    char msg_id[32];
    snprintf(msg_id, sizeof(msg_id), "%llu%08lx",
             (unsigned long long)(esp_timer_get_time() / 1000),
             (unsigned long)esp_random());
    cJSON_AddStringToObject(root, "msgId", msg_id);
    cJSON_AddStringToObject(root, "uid", uid ? uid : "");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        ESP_LOGE(TAG, "Could not build a request for %s", module);
        return false;
    }

    uint32_t before = heap_free();
    if (before < s_heap_min) s_heap_min = before;

    // Addressed explicitly: with several controllers connected, the poll
    // has to reach the one whose session it was set up for.
    mitm_proxy_inject_command_to(mac, json, strlen(json));
    cJSON_free(json);

    uint32_t after = heap_free();
    poll_note_activity();

    // A round that keeps eating heap is the failure this whole exercise is
    // about, so it is tracked rather than left to be inferred later.
    if (after < s_heap_min) s_heap_min = after;
    if (after < before) {
        ESP_LOGW(TAG, "  %s: heap %u -> %u", module,
                 (unsigned)before, (unsigned)after);
    }

    ESP_LOGI(TAG, "Asked %s for %s (heap %u, lowest %u)",
             mac, module, (unsigned)after, (unsigned)s_heap_min);
    return true;
}

static void send_poll_round(const char *mac, const char *uid)
{
    if (!mac || !mac[0]) return;

    s_round_seq++;
    s_heap_min = heap_free();
    s_round_active = true;
    poll_note_activity();
    ESP_LOGI(TAG, "Config poll round %u for %s (%u modules)",
             (unsigned)s_round_seq, mac, (unsigned)POLL_MODULE_COUNT);

    // Driven by POLL_MODULES rather than a second hand-maintained list, so a
    // module cannot be added to one and forgotten in the other.
    for (size_t i = 0; i < POLL_MODULE_COUNT; i++) {
        // The plan is read on every round like the rest: it is cached as
        // text (never a tree), so the reply costs little, and a write the
        // controller did not take (it closed the link instead) is
        // corrected on the next round rather than staying wrong.
        if (send_request(mac, uid, POLL_MODULES[i].name,
                         POLL_MODULES[i].flat)) {
            s_sent_ok++;
        } else {
            s_sent_fail++;
        }
        // Space the requests out rather than firing all of them at once.
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    ESP_LOGI(TAG, "Config poll round %u done (ok=%u fail=%u lowest heap %u)",
             (unsigned)s_round_seq, (unsigned)s_sent_ok,
             (unsigned)s_sent_fail, (unsigned)s_heap_min);
    poll_note_round_done();
}

// Set by config_poll_request() to trigger a round outside the timer.
static volatile bool s_poll_now = false;
static volatile bool s_cal_now = false;

void config_poll_request(void)
{
    s_poll_now = true;
}

void config_poll_request_calibration(void)
{
    s_cal_now = true;
}

static void poll_task(void *arg)
{
    int elapsed = 0;
    int cal_elapsed = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed++;
        cal_elapsed++;
        run_hooks();

        // A round that stopped making progress (reported, not reset).
        if (s_round_active && s_last_activity_ms &&
            (uint32_t)(esp_timer_get_time() / 1000) - s_last_activity_ms > 45000) {
            ESP_LOGE(TAG, "Poll round %u idle for over 45 s (heap %u, lowest %u)",
                     (unsigned)s_round_seq, (unsigned)heap_free(), (unsigned)s_heap_min);
            s_round_active = false;
        }

        // Calibration on its own schedule, so it keeps working even
        // where the full round is unsafe. Every two minutes is enough to
        // notice a change made from the vendor app without adding
        // meaningful traffic.
        if ((s_cal_now || cal_elapsed >= 120) && s_mac[0]) {
            s_cal_now = false;
            cal_elapsed = 0;
            send_request(s_mac, s_uid, "calibration", true);
            // The alarm block on the same schedule, for the same reason:
            // it has to be known before a single threshold can be changed,
            // because the controller only accepts the block whole.
            vTaskDelay(pdMS_TO_TICKS(1500));
            send_request(s_mac, s_uid, "alarm", true);
        }

        bool due = s_poll_now || elapsed >= SB_CONFIG_POLL_INTERVAL_S;
        if (!due) continue;

        s_poll_now = false;
        elapsed = 0;

        if (s_mac[0]) {
            send_poll_round(s_mac, s_uid);
        }
    }
}

void config_poll_start(void)
{
    if (s_running) return;
    s_running = true;
    // The hooks publish to MQTT and build JSON: 5 KB rather than 4.
    // 6144: ~1.5 KB was left at 5120 after the first poll round; the
    // hooks (time sync, bridge state) run here too.
    xTaskCreate(poll_task, "config_poll", 6144, NULL, 3, NULL);
    ESP_LOGI(TAG, "Config poll task started (every %d s)", SB_CONFIG_POLL_INTERVAL_S);
}