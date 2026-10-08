#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "sb_config.h"
#include "plan_text.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "device_registry.h"
#include "device_cache.h"

static const char *TAG = "device_cache";

// Modules whose full block is kept for command building.
//
// "calibration" is here for the same reason as the rest: the controller
// discards a partial block, so changing one offset means sending all
// four, and the other three have to come from somewhere.
//
// "target" is the day cycle block, also addressed flat and also sent whole.
// It is not a device module, so it has no [fan, light, ...] entry of its
// own here — only the cached object matters, which is all the command
// builder and the publisher read.
static const char *CACHED_MODULES[] = {
    "light", "light2", "fan", "blower",
    "heater", "humidifier", "dehumidifier",
    "calibration",
    "target",
    // Alarm thresholds and switches, keyPath ["alarm"]. Sent whole like
    // the two above, so a single change needs every other field.
    "alarm",
    // Not a command target: kept so the web interface can show what the
    // controller reports about itself — firmware and hardware versions,
    // build time, uptime, restart count — between status frames.
    "sys",
};
#define MODULE_COUNT (sizeof(CACHED_MODULES) / sizeof(CACHED_MODULES[0]))

// Modules that report a level worth remembering across an off period.
static const char *LEVEL_MODULES[] = { "light", "light2", "fan", "blower" };
#define LEVEL_COUNT (sizeof(LEVEL_MODULES) / sizeof(LEVEL_MODULES[0]))

typedef struct {
    bool   in_use;
    char   mac[DEV_MAC_LEN];
    cJSON *blocks[MODULE_COUNT];
    int    last_level[MODULE_COUNT];
    cJSON *sensors;
    cJSON *outlets;
    // Whether the temperature/humidity sensor cleaning cycle is running,
    // and when that was last decided.
    //
    // Tracked here rather than being read straight from the controller
    // because the controller does not report it in the frames the bridge
    // polls. A separate "have we ever been told" flag is needed alongside
    // the value: an unknown state and a known-OFF are different, and
    // conflating them makes the Home Assistant switch revert to OFF the
    // moment a frame arrives without the field.
    bool   heating;
    bool   heating_known;
    int64_t heating_changed_us;
    // What the controller reports in getDevSta.sensorHeating:
    // phase 0 idle, 1 cleaning, 2 cooldown; remain_s counts down within the
    // phase. Stamped so the countdown can be continued between frames.
    int    clean_phase;
    int    clean_remain_s;
    int64_t clean_stamp_us;
    bool   clean_reported;     // the controller has sent the block at least once
    // Grow plan, keyPath ["plan"]: {enabled, stage:[...]}. Each stage is
    // kept as its own compact JSON string, never as a tree of the whole
    // plan: five stages are ~5 KB of text but ~20 KB of cJSON nodes.
    char  *plan_stage[SB_PLAN_MAX_STAGES];
    int    plan_count;         // -1 unknown (never reported)
    int    plan_enabled;       // -1 unknown
    // Fan/blower mode bookkeeping for status frames without modeType
    // (device_cache_status_mode_absent): [0] fan, [1] blower.
    int64_t mode_set_us[2];    // last time a command changed modeType
    bool    mode_seen[2];      // a status frame has carried modeType
} cache_slot_t;

static cache_slot_t s_slots[SB_MAX_DEVICES];
static SemaphoreHandle_t s_lock = NULL;

// Change counter for the web interface: bumped only when a cached value
// actually changes, so an open page can ask "anything new?" cheaply
// (GET /control/ver) and fetch the full feed only then.
static volatile uint32_t s_ver = 1;
static inline void bump(void) { s_ver++; }
uint32_t device_cache_version(void) { return s_ver; }
void device_cache_bump(void) { bump(); }

void device_cache_lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void device_cache_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

void device_cache_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    memset(s_slots, 0, sizeof(s_slots));
    ESP_LOGI(TAG, "Device cache ready (%d devices x %u modules)",
             SB_MAX_DEVICES, (unsigned)MODULE_COUNT);
}

static int module_index(const char *module)
{
    for (size_t i = 0; i < MODULE_COUNT; i++) {
        if (strcmp(CACHED_MODULES[i], module) == 0) return (int)i;
    }
    return -1;
}

static bool tracks_level(const char *module)
{
    for (size_t i = 0; i < LEVEL_COUNT; i++) {
        if (strcmp(LEVEL_MODULES[i], module) == 0) return true;
    }
    return false;
}

// Caller must hold the lock.
static cache_slot_t *slot_for(const char *mac, bool create)
{
    if (!mac || !mac[0]) return NULL;

    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        if (s_slots[i].in_use && strcasecmp(s_slots[i].mac, mac) == 0) {
            return &s_slots[i];
        }
    }
    if (!create) return NULL;

    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        if (s_slots[i].in_use) continue;
        memset(&s_slots[i], 0, sizeof(s_slots[i]));
        s_slots[i].in_use = true;
        s_slots[i].plan_enabled = -1;
        s_slots[i].plan_count = -1;
        strncpy(s_slots[i].mac, mac, DEV_MAC_LEN - 1);
        return &s_slots[i];
    }
    return NULL;
}

// Copies every key of src into dst, replacing existing ones. This is the
// merge that keeps cached schedule and mode fields alive when a minimal
// status frame arrives carrying only {on, level}.
// Value of one of two alias keys as a number (true/false count as 1/0).
static double alias_num(cJSON *o, const char *a, const char *b, bool *has)
{
    cJSON *v = cJSON_GetObjectItem(o, a);
    if (!v) v = cJSON_GetObjectItem(o, b);
    *has = v != NULL;
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsTrue(v)) return 1;
    return 0;
}

// Equal as far as the web interface is concerned: every key except the
// on/level aliases compared as JSON, the aliases by value.
static bool same_values(cJSON *a, cJSON *b)
{
    bool ha, hb;
    double x = alias_num(a, "on", "mOnOff", &ha), y = alias_num(b, "on", "mOnOff", &hb);
    if (ha != hb || x != y) return false;
    x = alias_num(a, "level", "mLevel", &ha); y = alias_num(b, "level", "mLevel", &hb);
    if (ha != hb || x != y) return false;
    static const char *const ALIAS[] = { "on", "mOnOff", "level", "mLevel" };
    for (int pass = 0; pass < 2; pass++) {
        cJSON *p = pass ? b : a, *q = pass ? a : b, *it = NULL;
        cJSON_ArrayForEach(it, p) {
            if (!it->string) continue;
            bool alias = false;
            for (int k = 0; k < 4; k++) if (strcmp(it->string, ALIAS[k]) == 0) alias = true;
            if (alias) continue;
            cJSON *o = cJSON_GetObjectItem(q, it->string);
            if (!o || !cJSON_Compare(it, o, true)) return false;
        }
    }
    return true;
}

// Returns whether anything differed.
static bool merge_into(cJSON *dst, cJSON *src)
{
    bool changed = false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, src) {
        if (!item->string) continue;
        cJSON *old = cJSON_GetObjectItem(dst, item->string);
        if (old && cJSON_Compare(old, item, true)) continue;
        changed = true;
        cJSON_DeleteItemFromObject(dst, item->string);
        cJSON_AddItemToObject(dst, item->string, cJSON_Duplicate(item, true));
    }
    return changed;
}

void device_cache_merge(const char *mac, const char *module, cJSON *block)
{
    if (!cJSON_IsObject(block)) return;
    int mi = module_index(module);
    if (mi < 0) return;

    device_cache_lock();

    cache_slot_t *s = slot_for(mac, true);
    if (!s) {
        device_cache_unlock();
        return;
    }

    // "sys" carries the controller's clock and uptime, which change with
    // every report; counting those would make every open page reload all
    // the time. They still refresh with the next real change.
    bool counts = strcmp(module, "sys") != 0;
    if (!s->blocks[mi]) {
        s->blocks[mi] = cJSON_Duplicate(block, true);
        if (counts) bump();
    } else {
        // An alias swap (on <-> mOnOff) is not a change by itself; the
        // value comparison below decides.
        cJSON *before = counts ? cJSON_Duplicate(s->blocks[mi], true) : NULL;
        merge_into(s->blocks[mi], block);

        // Drop the stale alias when its counterpart is written.
        //
        // The controller reports on/off as "on" and "level" in status
        // frames, while commands the bridge sends use "mOnOff" and
        // "mLevel" — the same two values under two names. Both ended up
        // in the cache side by side, and every reader prefers "on", so
        // after an OFF command the cache held a fresh "mOnOff":0 next to
        // a stale "on":true from an earlier status frame, and the stale
        // one won.
        //
        // Observed as: switching the fan off in Home Assistant correctly
        // set the speed to 0, but the interface kept showing it as ON.
        //
        // Removing the alias that was NOT just written keeps exactly one
        // source of truth per value, whichever name it arrived under.
        if (cJSON_GetObjectItem(block, "mOnOff")) {
            cJSON_DeleteItemFromObject(s->blocks[mi], "on");
        } else if (cJSON_GetObjectItem(block, "on")) {
            cJSON_DeleteItemFromObject(s->blocks[mi], "mOnOff");
        }
        if (cJSON_GetObjectItem(block, "mLevel")) {
            cJSON_DeleteItemFromObject(s->blocks[mi], "level");
        } else if (cJSON_GetObjectItem(block, "level")) {
            cJSON_DeleteItemFromObject(s->blocks[mi], "mLevel");
        }
        if (before) {
            if (!same_values(before, s->blocks[mi])) bump();
            cJSON_Delete(before);
        }
    }

    // The controller reports zero while a module is off, so without
    // remembering the previous value an OFF -> ON command would bring it
    // back at level zero.
    if (tracks_level(module)) {
        cJSON *lvl = cJSON_GetObjectItem(block, "level");
        if (!lvl) lvl = cJSON_GetObjectItem(block, "mLevel");
        if (cJSON_IsNumber(lvl) && lvl->valuedouble > 0) {
            s->last_level[mi] = (int)lvl->valuedouble;
        }
    }

    device_cache_unlock();
}

void device_cache_replace(const char *mac, const char *module, cJSON *block)
{
    if (!cJSON_IsObject(block)) return;
    int mi = module_index(module);
    if (mi < 0) return;
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, true);
    if (s) {
        if (!s->blocks[mi] || !cJSON_Compare(s->blocks[mi], block, true)) bump();
        if (s->blocks[mi]) cJSON_Delete(s->blocks[mi]);
        s->blocks[mi] = cJSON_Duplicate(block, true);
    }
    device_cache_unlock();
}

static int fan_index(const char *module)
{
    if (strcmp(module, "fan") == 0) return 0;
    if (strcmp(module, "blower") == 0) return 1;
    return -1;
}

void device_cache_note_mode_command(const char *mac, const char *module)
{
    int fi = fan_index(module);
    if (fi < 0) return;
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, true);
    if (s) s->mode_set_us[fi] = esp_timer_get_time();
    device_cache_unlock();
}

int device_cache_status_mode(const char *mac, const char *module, bool frame_has_mode)
{
    int fi = fan_index(module);
    if (fi < 0) return MODE_FRAME_USE;
    int r = MODE_FRAME_USE;
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, true);
    if (s) {
        if (frame_has_mode) s->mode_seen[fi] = true;
        int64_t age = esp_timer_get_time() - s->mode_set_us[fi];
        if (s->mode_set_us[fi] && age < 15LL * 1000000) {
            // A command changed the mode moments ago. A frame built before
            // the controller applied it carries the old mode (or none) and
            // would flip the cache back -- the next command would then
            // send the old mode to the controller. Its mode is ignored.
            r = MODE_FRAME_IGNORE;
        } else if (!frame_has_mode && s->mode_seen[fi]) {
            // This controller reports the mode when it is not Manual, so an
            // absent one means Manual. (A controller never seen to report
            // it is left alone.)
            r = MODE_FRAME_MANUAL;
        }
    }
    device_cache_unlock();
    return r;
}

void device_cache_merge_data(const char *mac, cJSON *data)
{
    if (!cJSON_IsObject(data)) return;
    for (size_t i = 0; i < MODULE_COUNT; i++) {
        cJSON *block = cJSON_GetObjectItem(data, CACHED_MODULES[i]);
        if (cJSON_IsObject(block)) device_cache_merge(mac, CACHED_MODULES[i], block);
    }
    // The plan is cached from the frame text by device_cache_set_plan_text
    // (see mitm_proxy), never from this tree.
}

// --- Grow plan ---

static void plan_clear(cache_slot_t *s)
{
    for (int k = 0; k < SB_PLAN_MAX_STAGES; k++) { free(s->plan_stage[k]); s->plan_stage[k] = NULL; }
    s->plan_count = 0;
}

bool device_cache_set_plan_text(const char *mac, const char *plan, size_t len)
{
    if (!plan || len < 2 || plan[0] != '{') return false;
    long en = jtext_int(plan, len, "enabled", -1);
    const char *arr;
    size_t al;
    bool has_stages = jtext_find(plan, len, "stage", &arr, &al) && al >= 2 && arr[0] == '[';

    // Split the array into stage texts first, outside the lock.
    char *parts[SB_PLAN_MAX_STAGES] = {0};
    int n = 0;
    bool ok = true;
    if (has_stages) {
        size_t i = 1;
        while (i < al) {
            while (i < al && (arr[i] == ' ' || arr[i] == ',' || arr[i] == '\n' || arr[i] == '\r' || arr[i] == '\t')) i++;
            if (i >= al || arr[i] == ']') break;
            size_t vl = jtext_value_len(arr + i, al - i);
            if (!vl || arr[i] != '{') { ok = false; break; }
            if (n >= SB_PLAN_MAX_STAGES) {
                ESP_LOGW(TAG, "%s: plan has more than %d stages; the rest is not cached",
                         mac, SB_PLAN_MAX_STAGES);
                break;
            }
            parts[n] = malloc(vl + 1);
            if (!parts[n]) { ok = false; break; }
            memcpy(parts[n], arr + i, vl);
            parts[n][vl] = '\0';
            n++;
            i += vl;
        }
    }
    if (!ok) {
        for (int k = 0; k < n; k++) free(parts[k]);
        return false;
    }

    device_cache_lock();
    cache_slot_t *s = slot_for(mac, true);
    if (s) {
        bump();
        if (en >= 0) s->plan_enabled = en ? 1 : 0;
        if (has_stages) {
            plan_clear(s);
            for (int k = 0; k < n; k++) s->plan_stage[k] = parts[k];
            s->plan_count = n;
        }
    } else {
        for (int k = 0; k < n; k++) free(parts[k]);
    }
    device_cache_unlock();
    return s != NULL;
}

void device_cache_set_plan_enabled(const char *mac, int enabled)
{
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, true);
    if (s && s->plan_enabled != (enabled ? 1 : 0)) { s->plan_enabled = enabled ? 1 : 0; bump(); }
    device_cache_unlock();
}

int device_cache_plan_enabled(const char *mac)
{
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, false);
    int en = s ? s->plan_enabled : -1;
    device_cache_unlock();
    return en;
}

int device_cache_plan_count(const char *mac)
{
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, false);
    int n = s ? s->plan_count : -1;
    device_cache_unlock();
    return n;
}

char *device_cache_plan_stage_dup(const char *mac, int i)
{
    char *out = NULL;
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, false);
    if (s && i >= 0 && i < s->plan_count && s->plan_stage[i]) out = strdup(s->plan_stage[i]);
    device_cache_unlock();
    return out;
}

bool device_cache_plan_set_stages(const char *mac, char **stages, int n)
{
    if (n < 0 || n > SB_PLAN_MAX_STAGES) return false;
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, true);
    if (s) {
        plan_clear(s);
        for (int k = 0; k < n; k++) s->plan_stage[k] = stages[k];
        s->plan_count = n;
        bump();
    }
    device_cache_unlock();
    if (!s) for (int k = 0; k < n; k++) free(stages[k]);
    return s != NULL;
}

size_t device_cache_plan_text(const char *mac, char *out, size_t n, int *enabled)
{
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, false);
    size_t l = 0;
    if (s && s->plan_count >= 0) {
        // {"stage":[s0,s1,...]}
        bool fit = true;
        size_t need = 12;
        for (int k = 0; k < s->plan_count; k++) need += strlen(s->plan_stage[k]) + 1;
        if (need >= n) fit = false;
        if (fit) {
            l = (size_t)snprintf(out, n, "{\"stage\":[");
            for (int k = 0; k < s->plan_count; k++) {
                if (k) out[l++] = ',';
                size_t sl = strlen(s->plan_stage[k]);
                memcpy(out + l, s->plan_stage[k], sl);
                l += sl;
            }
            out[l++] = ']';
            out[l++] = '}';
            out[l] = '\0';
        }
    }
    if (enabled) *enabled = s ? s->plan_enabled : -1;
    device_cache_unlock();
    return l;
}

bool device_cache_plan_known(const char *mac)
{
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, false);
    bool k = s && s->plan_count >= 0;
    device_cache_unlock();
    return k;
}

cJSON *device_cache_get(const char *mac, const char *module)
{
    int mi = module_index(module);
    if (mi < 0) return NULL;
    cache_slot_t *s = slot_for(mac, false);
    return s ? s->blocks[mi] : NULL;
}

int device_cache_last_level(const char *mac, const char *module, int fallback)
{
    int mi = module_index(module);
    if (mi < 0) return fallback;

    device_cache_lock();
    cache_slot_t *s = slot_for(mac, false);
    int v = s ? s->last_level[mi] : 0;
    device_cache_unlock();

    return v > 0 ? v : fallback;
}

void device_cache_set_live(const char *mac, cJSON *sensors, cJSON *outlets)
{
    device_cache_lock();

    cache_slot_t *s = slot_for(mac, true);
    if (s) {
        if (cJSON_IsObject(sensors)) {
            if (!s->sensors || !cJSON_Compare(s->sensors, sensors, true)) bump();
            if (s->sensors) cJSON_Delete(s->sensors);
            s->sensors = cJSON_Duplicate(sensors, true);
        }
        if (cJSON_IsObject(outlets)) {
            if (!s->outlets || !cJSON_Compare(s->outlets, outlets, true)) bump();
            if (s->outlets) cJSON_Delete(s->outlets);
            s->outlets = cJSON_Duplicate(outlets, true);
        }
    }

    device_cache_unlock();
}

cJSON *device_cache_sensors(const char *mac)
{
    cache_slot_t *s = slot_for(mac, false);
    return s ? s->sensors : NULL;
}

cJSON *device_cache_outlets(const char *mac)
{
    cache_slot_t *s = slot_for(mac, false);
    return s ? s->outlets : NULL;
}

void device_cache_set_sensor_cleaning(const char *mac, bool on)
{
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, true);
    if (s) {
        // The timestamp only moves on an actual change, so "how long has
        // this been heating" stays true across the many frames that repeat
        // the same value.
        if (!s->heating_known || s->heating != on) {
            s->heating = on;
            s->heating_known = true;
            s->heating_changed_us = esp_timer_get_time();
            bump();
        }
    }
    device_cache_unlock();
}

void device_cache_set_sensor_cleaning_phase(const char *mac, int phase, int remain_s)
{
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, true);
    if (s) {
        // The remaining time counts down on the page itself; only a phase
        // change is news.
        if (!s->clean_reported || s->clean_phase != phase) bump();
        s->clean_phase = phase;
        s->clean_remain_s = remain_s < 0 ? 0 : remain_s;
        s->clean_stamp_us = esp_timer_get_time();
        s->clean_reported = true;
        // The switch follows the phase: on only while actually cleaning.
        // The cooldown already counts as "off" -- the app shows the same.
        bool on = (phase == 1);
        if (!s->heating_known || s->heating != on) {
            s->heating = on;
            s->heating_known = true;
            s->heating_changed_us = esp_timer_get_time();
        }
    }
    device_cache_unlock();
}

bool device_cache_sensor_cleaning_phase(const char *mac, int *phase, int *remain_s)
{
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, false);
    bool ok = s && s->clean_reported;
    int ph = ok ? s->clean_phase : 0;
    int rem = 0;
    if (ok && ph != 0) {
        // Continue the countdown locally between frames.
        int64_t el = (esp_timer_get_time() - s->clean_stamp_us) / 1000000;
        rem = s->clean_remain_s - (int)el;
        if (rem < 0) rem = 0;
    }
    if (phase) *phase = ph;
    if (remain_s) *remain_s = rem;
    device_cache_unlock();
    return ok;
}

bool device_cache_sensor_cleaning(const char *mac, bool *known, int64_t *changed_us)
{
    device_cache_lock();
    cache_slot_t *s = slot_for(mac, false);
    if (known)      *known = s ? s->heating_known : false;
    if (changed_us) *changed_us = s ? s->heating_changed_us : 0;
    bool on = s ? s->heating : false;
    device_cache_unlock();
    return on;
}

void device_cache_clear(const char *mac)
{
    device_cache_lock();

    cache_slot_t *s = slot_for(mac, false);
    if (s) {
        for (size_t i = 0; i < MODULE_COUNT; i++) {
            if (s->blocks[i]) {
                cJSON_Delete(s->blocks[i]);
                s->blocks[i] = NULL;
            }
        }
        if (s->sensors) { cJSON_Delete(s->sensors); s->sensors = NULL; }
        if (s->outlets) { cJSON_Delete(s->outlets); s->outlets = NULL; }
        for (int k = 0; k < SB_PLAN_MAX_STAGES; k++) free(s->plan_stage[k]);
        memset(s, 0, sizeof(*s));
        bump();
    }

    device_cache_unlock();
}
