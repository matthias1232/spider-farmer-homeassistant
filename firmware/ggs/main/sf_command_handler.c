#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sb_config.h"
#include "provisioning.h"
#include "device_cache.h"
#include "device_registry.h"
#include "mitm_proxy.h"
#include "sf_normalizer.h"
#include "sf_alarm.h"
#include "config_poll.h"
#include "plan_text.h"
#include "plan_templates.h"
#include "plan_store.h"
#include <math.h>

#include "sf_command_handler.h"

static const char *TAG = "sf_command";

// ---------------------------------------------------------------------------
// Mode tables, confirmed against the Spider Farmer app. Order matches the
// app's dropdown so the Home Assistant list reads the same way.
// ---------------------------------------------------------------------------
// Named so it can appear in a function signature. An anonymous struct
// there is not visible outside its own declaration, which makes every
// call site a type mismatch.
typedef struct { const char *label; int type; } mode_entry_t;

static const mode_entry_t FAN_MODES[] = {
    { "Manual",                              0  },
    { "Schedule",                            1  },
    { "Cycle",                               2  },
    { "Environment: Prioritize temperature", 7  },
    { "Environment: Prioritize humidity",    8  },
    { "Environment: Temperature only",       3  },
    { "Environment: Humidity only",          4  },
    { "Environment: Temperature & humidity", 13 },
};
#define FAN_MODE_COUNT (sizeof(FAN_MODES) / sizeof(FAN_MODES[0]))

// Same modeType space, just without the "Environment: " prefix — the
// card name already implies it.
static const mode_entry_t FAN_ENV_MODES[] = {
    { "Prioritize temperature", 7  },
    { "Prioritize humidity",    8  },
    { "Temperature only",       3  },
    { "Humidity only",          4  },
    { "Temperature & humidity", 13 },
};
#define FAN_ENV_COUNT (sizeof(FAN_ENV_MODES) / sizeof(FAN_ENV_MODES[0]))

static const mode_entry_t LIGHT_MODES[] = {
    { "Manual",   0  },
    { "Schedule", 1  },
    { "PPFD",     12 },
};
#define LIGHT_MODE_COUNT (sizeof(LIGHT_MODES) / sizeof(LIGHT_MODES[0]))

static int lookup_mode(const char *label, const mode_entry_t *table, size_t count)
{
    if (!label) return -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(table[i].label, label) == 0) return table[i].type;
    }
    return -1;
}

static int onoff_val(const char *v)
{
    if (!v) return 0;
    return (strcasecmp(v, "ON") == 0 || strcmp(v, "1") == 0 ||
            strcasecmp(v, "true") == 0) ? 1 : 0;
}

static int clamp_int(const char *value, int lo, int hi, bool *ok)
{
    char *end = NULL;
    double d = strtod(value, &end);
    if (end == value) { if (ok) *ok = false; return lo; }
    if (ok) *ok = true;
    int v = (int)d;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

// Same as clamp_int but keeps the fraction, for the day cycle targets where
// the app works in tenths and a truncated value would come back changed.
static double clamp_double(const char *value, double lo, double hi, bool *ok)
{
    char *end = NULL;
    double d = strtod(value, &end);
    if (end == value) { if (ok) *ok = false; return lo; }
    if (ok) *ok = true;
    if (d < lo) d = lo;
    if (d > hi) d = hi;
    return d;
}

// The message id in the app's own format: milliseconds since the Unix
// epoch, 13 digits, e.g. "1791315395962".
//
// The controller's reply carries this id up to the cloud. With the app's
// format the cloud accepts the reply and the change shows in the Spider
// Farmer app; the earlier uptime-plus-hex id ("122886eb7a645f") was
// relayed just the same but evidently not matched, so settings made from
// the bridge took effect on the controller yet never appeared in the app.
// Confirmed by sending a setConfigField with an epoch id by hand: it was
// applied and mirrored back.
//
// Kept strictly increasing, so two commands in the same millisecond never
// share an id. Without a synced clock there is no valid epoch to use, and
// the uptime form is the fallback.
static void make_msg_id(char *out, size_t out_sz)
{
    static uint64_t s_last = 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec > 1600000000) {
        uint64_t ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
        if (ms <= s_last) ms = s_last + 1;
        s_last = ms;
        snprintf(out, out_sz, "%llu", (unsigned long long)ms);
        return;
    }
    uint64_t up = (uint64_t)(esp_timer_get_time() / 1000);
    snprintf(out, out_sz, "%llu%08lx",
             (unsigned long long)up, (unsigned long)esp_random());
}

// Wraps a module block in the setConfigField envelope the controller
// expects. Takes ownership of obj.
static cJSON *build_command(const char *mac, const char *uid,
                            const char *domain, const char *module, cJSON *obj)
{
    char msg_id[32];
    make_msg_id(msg_id, sizeof(msg_id));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "method", "setConfigField");
    cJSON_AddStringToObject(root, "pid", mac);

    cJSON *params = cJSON_CreateObject();
    cJSON *keypath = cJSON_CreateArray();
    cJSON_AddItemToArray(keypath, cJSON_CreateString(domain));
    cJSON_AddItemToArray(keypath, cJSON_CreateString(module));
    cJSON_AddItemToObject(params, "keyPath", keypath);
    cJSON_AddItemToObject(params, module, obj);
    cJSON_AddItemToObject(root, "params", params);

    cJSON_AddStringToObject(root, "msgId", msg_id);
    cJSON_AddStringToObject(root, "uid", uid);
    return root;
}

// Same, but with a single-element keyPath.
//
// Calibration is addressed as keyPath ["calibration"], with no "device"
// prefix — confirmed by capturing what the vendor app sends. Guessing a
// two-element path here produces a command the controller silently
// ignores, which is indistinguishable from a broken connection.
static cJSON *build_command_flat(const char *mac, const char *uid,
                                 const char *module, cJSON *obj)
{
    char msg_id[32];
    make_msg_id(msg_id, sizeof(msg_id));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "method", "setConfigField");
    cJSON_AddStringToObject(root, "pid", mac);

    cJSON *params = cJSON_CreateObject();
    cJSON *keypath = cJSON_CreateArray();
    cJSON_AddItemToArray(keypath, cJSON_CreateString(module));
    cJSON_AddItemToObject(params, "keyPath", keypath);
    cJSON_AddItemToObject(params, module, obj);
    cJSON_AddItemToObject(root, "params", params);

    cJSON_AddStringToObject(root, "msgId", msg_id);
    cJSON_AddStringToObject(root, "uid", uid);
    return root;
}

// Same, but for a method that is not a config write at all.
//
// The sensor-cleaning cycle is setSensorHeating with params {"on":0|1} and
// nothing else -- no keyPath, no device block. Captured from the vendor app:
//   {"method":"setSensorHeating","params":{"on":1},"msgId":...,
//    "pid":"D0CF137A60B8","uid":"162606"}
// The msgId is milliseconds since the epoch plus a short suffix, not the
// esp_timer stamp the config writes use.
static cJSON *build_command_heating(const char *mac, const char *uid, int on)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
    char msg_id[40];
    snprintf(msg_id, sizeof(msg_id), "%llu%04x",
             (unsigned long long)ms, (unsigned)(esp_random() & 0xffff));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "method", "setSensorHeating");
    cJSON_AddStringToObject(root, "pid", mac);

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "on", on ? 1 : 0);
    cJSON_AddItemToObject(root, "params", params);

    cJSON_AddStringToObject(root, "msgId", msg_id);
    cJSON_AddStringToObject(root, "uid", uid);
    return root;
}

// Default timePeriod used by the simple on/off paths.
static cJSON *default_time_period(void)
{
    cJSON *arr = cJSON_CreateArray();
    cJSON *tp = cJSON_CreateObject();
    cJSON_AddNumberToObject(tp, "weekmask", 127);
    cJSON_AddItemToArray(arr, tp);
    return arr;
}

// Returns a working copy of the cached block, or a synthesized default
// when nothing has been cached yet. Without a fallback the controller
// would reject the partial command and the user would see no effect at
// all — the hardest kind of failure to diagnose.
static cJSON *fan_block_copy(const char *mac, const char *field)
{
    device_cache_lock();
    cJSON *cached = device_cache_get(mac, field);
    cJSON *block = cached ? cJSON_Duplicate(cached, true) : NULL;
    device_cache_unlock();

    if (block) {
        // Status frames report on/level, commands use mOnOff/mLevel —
        // the same two values under two names. The cache can hold
        // either, so normalise to the command spelling here and drop
        // the aliases, rather than sending both and letting the
        // controller pick which one wins.
        cJSON *v = cJSON_GetObjectItem(block, "on");
        if (v && !cJSON_GetObjectItem(block, "mOnOff")) {
            cJSON_AddNumberToObject(block, "mOnOff",
                                    cJSON_IsNumber(v) ? v->valuedouble
                                                      : (cJSON_IsTrue(v) ? 1 : 0));
        }
        v = cJSON_GetObjectItem(block, "level");
        if (cJSON_IsNumber(v) && !cJSON_GetObjectItem(block, "mLevel")) {
            cJSON_AddNumberToObject(block, "mLevel", v->valuedouble);
        }
        cJSON_DeleteItemFromObject(block, "on");
        cJSON_DeleteItemFromObject(block, "level");
        return block;
    }

    ESP_LOGI(TAG, "%s cache empty — using synthesized defaults", field);
    block = cJSON_CreateObject();
    cJSON_AddNumberToObject(block, "modeType", 0);
    cJSON_AddNumberToObject(block, "mOnOff", 0);
    cJSON_AddNumberToObject(block, "mLevel", 1);
    cJSON_AddNumberToObject(block, "minSpeed", 0);
    cJSON_AddNumberToObject(block, "maxSpeed", 1);

    // The two modules do not share a field set. Captured traffic shows
    // shakeLevel and natural only on the circulation fan, and closeCO2
    // only on the exhaust blower. Sending the wrong ones was harmless
    // only as long as this block was never built from real data.
    if (strcmp(field, "blower") == 0) {
        cJSON_AddNumberToObject(block, "closeCO2", 0);
    } else {
        cJSON_AddNumberToObject(block, "shakeLevel", 0);
        cJSON_AddNumberToObject(block, "natural", 0);
    }

    cJSON *tps = cJSON_CreateArray();
    cJSON *tp = cJSON_CreateObject();
    cJSON_AddNumberToObject(tp, "enabled", 1);
    cJSON_AddNumberToObject(tp, "weekmask", 127);
    cJSON_AddNumberToObject(tp, "startTime", 0);
    cJSON_AddNumberToObject(tp, "endTime", 0);
    cJSON_AddItemToArray(tps, tp);
    cJSON_AddItemToObject(block, "timePeriod", tps);

    cJSON *ct = cJSON_CreateObject();
    cJSON_AddNumberToObject(ct, "weekmask", 127);
    cJSON_AddNumberToObject(ct, "startTime", 0);
    cJSON_AddNumberToObject(ct, "openDur", 0);
    cJSON_AddNumberToObject(ct, "closeDur", 0);
    cJSON_AddNumberToObject(ct, "times", 1);
    cJSON_AddItemToObject(block, "cycleTime", ct);

    return block;
}

static cJSON *light_block_copy(const char *mac, const char *field)
{
    device_cache_lock();
    cJSON *cached = device_cache_get(mac, field);
    cJSON *block = cached ? cJSON_Duplicate(cached, true) : NULL;
    device_cache_unlock();

    if (block) return block;

    ESP_LOGI(TAG, "%s cache empty — using synthesized defaults", field);
    block = cJSON_CreateObject();
    cJSON_AddNumberToObject(block, "modeType", 0);
    cJSON_AddNumberToObject(block, "lastAutoModeType", 0);
    cJSON_AddNumberToObject(block, "mOnOff", 0);
    cJSON_AddNumberToObject(block, "mLevel", 0);
    cJSON_AddNumberToObject(block, "darkTemp", 0);
    cJSON_AddNumberToObject(block, "offTemp", 0);
    cJSON_AddNumberToObject(block, "ppfdMinBrightness", 0);
    cJSON_AddNumberToObject(block, "ppfdMaxBrightness", 100);

    for (int i = 0; i < 2; i++) {
        cJSON *arr = cJSON_CreateArray();
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "enabled", i == 0 ? 1 : 0);
        cJSON_AddNumberToObject(p, "weekmask", 127);
        cJSON_AddNumberToObject(p, "startTime", 0);
        cJSON_AddNumberToObject(p, "endTime", 0);
        cJSON_AddNumberToObject(p, "brightness", 0);
        cJSON_AddNumberToObject(p, "fadeTime", 0);
        cJSON_AddItemToArray(arr, p);
        cJSON_AddItemToObject(block, i == 0 ? "timePeriod" : "ppfdPeriod", arr);
    }
    return block;
}

// Returns element 0 of an array field, creating the array and element if
// they are missing.
static cJSON *period_0(cJSON *block, const char *key)
{
    cJSON *arr = cJSON_GetObjectItem(block, key);
    if (!cJSON_IsArray(arr)) {
        cJSON_DeleteItemFromObject(block, key);
        arr = cJSON_CreateArray();
        cJSON_AddItemToObject(block, key, arr);
    }
    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_AddItemToArray(arr, cJSON_CreateObject());
    }
    return cJSON_GetArrayItem(arr, 0);
}

static void set_num(cJSON *obj, const char *key, double v)
{
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddNumberToObject(obj, key, v);
}

// Every fan write forces timePeriod[0].enabled = 1.
//
// Observed bug in the original: when the cache was seeded from a status
// frame that omitted "enabled", the command went out without it and the
// controller treated the schedule as disabled. The trigger then never
// fired until the user pressed Save in the Spider Farmer app, whose
// payload always includes enabled=1.
static void force_schedule_enabled(cJSON *block)
{
    cJSON *arr = cJSON_GetObjectItem(block, "timePeriod");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) return;
    cJSON *tp = cJSON_GetArrayItem(arr, 0);
    if (!cJSON_IsObject(tp)) return;
    if (!cJSON_GetObjectItem(tp, "enabled"))  cJSON_AddNumberToObject(tp, "enabled", 1);
    if (!cJSON_GetObjectItem(tp, "weekmask")) cJSON_AddNumberToObject(tp, "weekmask", 127);
}

// Switching a fan into Schedule, Cycle or Environment.
//
// The app always sends these with the module switched on (mOnOff 1, a
// non-zero mLevel) and with a complete timePeriod (startTime/endTime) and
// cycleTime. The bridge used to carry whatever the cache held -- after an
// OFF that was mOnOff 0 / mLevel 0, and the controller's own block often
// has no times at all ({"enabled":1,"weekmask":127}). The controller took
// the mode but the fan did not start until the app saved the mode again
// with its full block. mOnOff is the module's master switch in every mode,
// so it is armed here and the missing times are filled in.
// A short note about something the last command changed on its own, for
// the web page to show (read and cleared by sf_command_take_note()).
// Per task (thread-local): the web server, the MQTT task and the poll task
// all translate commands, and a shared buffer let one clear or overwrite
// another's note between translate and take.
static __thread char s_note[160];
const char *sf_command_take_note(char *out, size_t n)
{
    snprintf(out, n, "%s", s_note);
    s_note[0] = '\0';
    return out;
}

static bool is_env_mode(int mt)
{
    return mt == 3 || mt == 4 || mt == 7 || mt == 8 || mt == 13;
}

// Returns false (with a note) when the mode cannot work with the block's
// settings -- the command is then not sent.
static bool fan_arm_auto_mode(cJSON *block, const char *mac, const char *field, int mt)
{
    // Cycle needs a run and an off time. With none set the controller
    // would take the mode and the fan would never run.
    if (mt == 2) {
        cJSON *c = cJSON_GetObjectItem(block, "cycleTime");
        cJSON *od = c ? cJSON_GetObjectItem(c, "openDur") : NULL;
        cJSON *cd = c ? cJSON_GetObjectItem(c, "closeDur") : NULL;
        if (!cJSON_IsNumber(od) || od->valuedouble <= 0 ||
            !cJSON_IsNumber(cd) || cd->valuedouble <= 0) {
            snprintf(s_note, sizeof(s_note),
                     "Set the Cycle run and off times first -- with none the fan would never run");
            ESP_LOGW(TAG, "%s %s", field, s_note);
            return false;
        }
    }
    // Schedule speed Auto (maxSpeed 0) exists only in the Environment
    // modes. Leaving one with Auto set would hand the controller a speed it
    // does not accept in the new mode, so it becomes the lowest step: 1 on
    // the circulation fan, 25 % on the exhaust (its floor).
    if (!is_env_mode(mt)) {
        cJSON *ms = cJSON_GetObjectItem(block, "maxSpeed");
        if (cJSON_IsNumber(ms) && ms->valuedouble <= 0) {
            bool blower = strcmp(field, "blower") == 0;
            int to = blower ? 25 : 1;
            set_num(block, "maxSpeed", to);
            snprintf(s_note, sizeof(s_note),
                     "%s schedule speed was Auto, which only exists in Environment mode: set to %d%s",
                     blower ? "Fan Exhaust" : "Fan Circulation", to, blower ? " %" : "");
            ESP_LOGW(TAG, "%s", s_note);
        }
    }
    if (mt == 0) return true;
    set_num(block, "mOnOff", 1);
    cJSON *lv = cJSON_GetObjectItem(block, "mLevel");
    if (!cJSON_IsNumber(lv) || lv->valuedouble <= 0) {
        bool blower = strcmp(field, "blower") == 0;
        int last = device_cache_last_level(mac, field, blower ? 50 : 5);
        set_num(block, "mLevel", last);
    }
    cJSON *tp = period_0(block, "timePeriod");
    if (!cJSON_GetObjectItem(tp, "startTime")) cJSON_AddNumberToObject(tp, "startTime", 0);
    if (!cJSON_GetObjectItem(tp, "endTime"))   cJSON_AddNumberToObject(tp, "endTime", 0);
    set_num(tp, "enabled", 1);
    cJSON *ct = cJSON_GetObjectItem(block, "cycleTime");
    if (!cJSON_IsObject(ct)) {
        ct = cJSON_CreateObject();
        cJSON_AddItemToObject(block, "cycleTime", ct);
    }
    if (!cJSON_GetObjectItem(ct, "weekmask"))  cJSON_AddNumberToObject(ct, "weekmask", 127);
    if (!cJSON_GetObjectItem(ct, "startTime")) cJSON_AddNumberToObject(ct, "startTime", 0);
    if (!cJSON_GetObjectItem(ct, "openDur"))   cJSON_AddNumberToObject(ct, "openDur", 0);
    if (!cJSON_GetObjectItem(ct, "closeDur"))  cJSON_AddNumberToObject(ct, "closeDur", 0);
    if (!cJSON_GetObjectItem(ct, "times"))     cJSON_AddNumberToObject(ct, "times", 1);
    return true;
}

// ---------------------------------------------------------------------------
// Fan and blower subfields
// ---------------------------------------------------------------------------
static cJSON *handle_fan_subfield(const char *field, const char *subfield,
                                  const char *value, const char *mac,
                                  const char *uid)
{
    cJSON *block = fan_block_copy(mac, field);
    bool ok = true;
    // Fan Circulation runs 1-10. Fan Exhaust (blower) runs 25-100: the
    // controller only accepts that range for the blower, so anything below
    // 25 is rejected outright and the setting silently keeps its old value.
    // The discovery payload sets the same bounds in Home Assistant.
    bool is_circulation = (strcmp(field, "blower") != 0);

    if (strcmp(subfield, "preset_mode") == 0) {
        int mt = lookup_mode(value, FAN_MODES, FAN_MODE_COUNT);
        if (mt < 0) {
            ESP_LOGW(TAG, "Unknown fan preset_mode: %s", value);
            cJSON_Delete(block);
            return NULL;
        }
        set_num(block, "modeType", mt);
        if (!fan_arm_auto_mode(block, mac, field, mt)) { cJSON_Delete(block); return NULL; }

    } else if (strcmp(subfield, "env_submode") == 0) {
        int mt = lookup_mode(value, FAN_ENV_MODES, FAN_ENV_COUNT);
        if (mt < 0) {
            ESP_LOGW(TAG, "Unknown fan env_submode: %s", value);
            cJSON_Delete(block);
            return NULL;
        }
        set_num(block, "modeType", mt);
        if (!fan_arm_auto_mode(block, mac, field, mt)) { cJSON_Delete(block); return NULL; }

    } else if (strcmp(subfield, "main_mode") == 0) {
        // The top level of the app's two-step picker. Manual, Schedule
        // and Cycle map to one modeType each; "Environment" does not —
        // the wire has five environment values and no neutral one.
        //
        // Switching to Environment therefore reuses whichever variant
        // the controller already holds, and only falls back to
        // "Prioritize temperature" (7) when the current mode is not an
        // environment mode at all. Picking a fixed value here would
        // silently discard the user's sub-mode every time they left
        // Environment and came back.
        int mt;
        if (strcmp(value, "Manual") == 0)        mt = 0;
        else if (strcmp(value, "Schedule") == 0) mt = 1;
        else if (strcmp(value, "Cycle") == 0)    mt = 2;
        else if (strcmp(value, "Environment") == 0) {
            cJSON *cur = cJSON_GetObjectItem(block, "modeType");
            int c = cJSON_IsNumber(cur) ? (int)cur->valuedouble : 0;
            mt = (c == 3 || c == 4 || c == 7 || c == 8 || c == 13) ? c : 7;
        } else {
            ESP_LOGW(TAG, "Unknown fan main_mode: %s", value);
            cJSON_Delete(block);
            return NULL;
        }
        set_num(block, "modeType", mt);
        if (!fan_arm_auto_mode(block, mac, field, mt)) { cJSON_Delete(block); return NULL; }

    } else if (strcmp(subfield, "schedule_start") == 0) {
        set_num(period_0(block, "timePeriod"), "startTime", sf_hhmm_to_seconds(value));

    } else if (strcmp(subfield, "schedule_end") == 0) {
        set_num(period_0(block, "timePeriod"), "endTime", sf_hhmm_to_seconds(value));

    } else if (strcmp(subfield, "schedule_speed") == 0) {
        // Only maxSpeed, the "Gang" the scheduled modes run at.
        //
        // This also used to write mLevel, which made the manual speed
        // jump whenever the schedule speed was touched: the two are read
        // from different fields, but the controller accepts whichever it
        // is given, so a value meant for one mode silently changed the
        // other. In manual mode that is plainly wrong -- only the speed
        // above should have any effect there.
        //
        // mLevel stays reachable through "percentage", which is the
        // control the Home Assistant fan entity and the Speed slider both
        // drive, so no speed becomes unreachable.
        //
        // Accepted as a percent in steps of ten, matching what the app
        // shows and what the discovery payload offers, and scaled down
        // onto the wire. Auto is passed through as 0, which is the
        // controller's own "you choose" -- it does not collide with the
        // standby field's Off, because that is minSpeed, where 0 already
        // means off.
        // As the app offers it: the fan a step 1-10, the exhaust 25-100 %
        // in 1 % steps, plus Auto (0), which only the Environment modes
        // accept.
        int v;
        if (strcmp(value, "Auto") == 0 || strcmp(value, "auto") == 0 ||
            strcmp(value, "0") == 0) {
            cJSON *mtj = cJSON_GetObjectItem(block, "modeType");
            int mt = cJSON_IsNumber(mtj) ? (int)mtj->valuedouble : 0;
            if (!(mt == 3 || mt == 4 || mt == 7 || mt == 8 || mt == 13)) {
                ESP_LOGW(TAG, "%s schedule speed Auto is only available in Environment mode", field);
                cJSON_Delete(block);
                return NULL;
            }
            v = 0;
        } else {
            v = is_circulation ? clamp_int(value, 1, 10, &ok)
                               : clamp_int(value, 25, 100, &ok);
            if (!ok) { cJSON_Delete(block); return NULL; }
        }
        set_num(block, "maxSpeed", v);

    } else if (strcmp(subfield, "standby_speed") == 0) {
        // As the app offers it: Off, or a value in the module's standby
        // band. The blower's band is 25-39, narrower than its running
        // range -- standby there is a deliberate low setting, not a
        // fraction of the running speed.
        //
        // The circulation fan's standby is fixed at 0 and is not settable:
        // its wire range starts at 1, so there is no lower value to hold,
        // and every reachable value would be a running speed. A write is
        // refused rather than clamped, so nothing here silently becomes a
        // running speed.
        int v;
        if (!is_circulation) {
            if (strcmp(value, "Off") == 0 || strcmp(value, "off") == 0 ||
                strcmp(value, "0") == 0) {
                v = 0;
            } else {
                v = clamp_int(value, 25, 39, &ok);
                if (!ok) { cJSON_Delete(block); return NULL; }
            }
        } else if (strcmp(value, "Off") == 0 || strcmp(value, "off") == 0 ||
                   strcmp(value, "0") == 0) {
            v = 0;
        } else {
            ESP_LOGW(TAG, "standby_speed is fixed at 0 for the circulation fan");
            cJSON_Delete(block);
            return NULL;
        }
        set_num(block, "minSpeed", v);

    } else if (strcmp(subfield, "cycle_start") == 0) {
        cJSON *ct = cJSON_GetObjectItem(block, "cycleTime");
        if (!cJSON_IsObject(ct)) {
            ct = cJSON_CreateObject();
            cJSON_AddNumberToObject(ct, "weekmask", 127);
            cJSON_AddItemToObject(block, "cycleTime", ct);
        }
        set_num(ct, "startTime", sf_hhmm_to_seconds(value));

    } else if (strcmp(subfield, "cycle_run_minutes") == 0 ||
               strcmp(subfield, "cycle_off_minutes") == 0) {
        cJSON *ct = cJSON_GetObjectItem(block, "cycleTime");
        if (!cJSON_IsObject(ct)) {
            ct = cJSON_CreateObject();
            cJSON_AddNumberToObject(ct, "weekmask", 127);
            cJSON_AddItemToObject(block, "cycleTime", ct);
        }
        int v = clamp_int(value, 0, 1440, &ok);
        if (!ok) { cJSON_Delete(block); return NULL; }
        set_num(ct, subfield[6] == 'r' ? "openDur" : "closeDur", v * 60);

    } else if (strcmp(subfield, "cycle_run_time") == 0 ||
               strcmp(subfield, "cycle_off_time") == 0) {
        // HH:MM:SS, matching what the app shows. The minute entities
        // above write the same two controller fields; this one keeps the
        // seconds the app allows (a captured run time of 03h 02min 02s
        // is 10922 s, which whole minutes cannot express).
        cJSON *ct = cJSON_GetObjectItem(block, "cycleTime");
        if (!cJSON_IsObject(ct)) {
            ct = cJSON_CreateObject();
            cJSON_AddNumberToObject(ct, "weekmask", 127);
            cJSON_AddItemToObject(block, "cycleTime", ct);
        }
        int secs = sf_hhmmss_to_seconds(value);
        if (secs > 86400) secs = 86400;
        set_num(ct, subfield[6] == 'r' ? "openDur" : "closeDur", secs);

    } else if (strcmp(subfield, "cycle_times") == 0) {
        cJSON *ct = cJSON_GetObjectItem(block, "cycleTime");
        if (!cJSON_IsObject(ct)) {
            ct = cJSON_CreateObject();
            cJSON_AddNumberToObject(ct, "weekmask", 127);
            cJSON_AddItemToObject(block, "cycleTime", ct);
        }
        // The controller hard-caps at 100 regardless of cycle duration.
        int v = clamp_int(value, 1, 100, &ok);
        if (!ok) { cJSON_Delete(block); return NULL; }
        set_num(ct, "times", v);

    } else if (strcmp(subfield, "oscillation_level") == 0) {
        // Circulation fan only. The exhaust blower has no oscillating
        // head, and its blocks never carry shakeLevel — writing one
        // would add a field the controller does not know for that
        // module.
        if (!is_circulation) {
            ESP_LOGW(TAG, "oscillation_level is not available on %s", field);
            cJSON_Delete(block);
            return NULL;
        }
        // The level, 1-10. Choosing a level also switches oscillation on,
        // like the app (shakeLevel 0 is off). A 0 is taken as off.
        int lvl = clamp_int(value, 0, 10, &ok);
        if (!ok) { cJSON_Delete(block); return NULL; }
        set_num(block, "shakeLevel", lvl);

    } else if (strcmp(subfield, "oscillation") == 0) {
        // The switch: off writes 0, on restores the last level used.
        if (!is_circulation) { cJSON_Delete(block); return NULL; }
        int on = onoff_val(value);
        int lvl = 0;
        if (on) {
            cJSON *cur = cJSON_GetObjectItem(block, "shakeLevel");
            int c = cJSON_IsNumber(cur) ? (int)cur->valuedouble : 0;
            if (c > 0) lvl = c;
            else {
                device_registry_lock();
                device_entry_t *d = device_registry_find(mac);
                char slug[DEV_SLUG_LEN] = "";
                if (d) strncpy(slug, d->slug, sizeof(slug) - 1);
                device_registry_unlock();
                lvl = sf_osc_last(slug);
            }
        }
        set_num(block, "shakeLevel", lvl);

    } else if (strcmp(subfield, "natural_wind") == 0) {
        // Circulation fan only, same reasoning as oscillation above:
        // captured blower traffic never contains "natural".
        if (!is_circulation) {
            ESP_LOGW(TAG, "natural_wind is not available on %s", field);
            cJSON_Delete(block);
            return NULL;
        }
        set_num(block, "natural", onoff_val(value));

    } else if (strcmp(subfield, "close_co2") == 0) {
        // Exhaust blower only: closeCO2 appears in every captured blower
        // block and in none of the circulation fan's.
        //
        // Polarity confirmed, 1 = on: two captured commands two seconds
        // apart with modeType held at 0 and every other field identical,
        // differing only in closeCO2 — 0 when the app's CO2 switch was
        // turned off, 1 when it was turned on. An earlier reading of the
        // logs suggested the value tracked the mode (1 in Manual and
        // Cycle, 0 in Schedule and Environment); that was coincidence.
        if (is_circulation) {
            ESP_LOGW(TAG, "close_co2 is not available on %s", field);
            cJSON_Delete(block);
            return NULL;
        }
        set_num(block, "closeCO2", onoff_val(value));

    } else {
        cJSON_Delete(block);
        return NULL;
    }

    // Only outside Manual: in Manual mode the app leaves the schedule's
    // "enabled" as it is, and the on/off+speed path does the same. One rule
    // for both paths, so two commands in Manual never differ in shape.
    cJSON *mtv = cJSON_GetObjectItem(block, "modeType");
    if (cJSON_IsNumber(mtv) && (int)mtv->valuedouble != 0) force_schedule_enabled(block);
    return build_command(mac, uid, "device", field, block);
}

// ---------------------------------------------------------------------------
// Light subfields
// ---------------------------------------------------------------------------
static cJSON *handle_light_subfield(const char *field, const char *subfield,
                                    const char *value, const char *mac,
                                    const char *uid)
{
    cJSON *block = light_block_copy(mac, field);
    bool ok = true;

    // Dim and off thresholds, as the app offers them: Off, or a
    // temperature in degrees C at which the light dims or shuts down.
    // Off is the controller's own 0, which it reads as "never".
    if (strcmp(subfield, "dim_threshold") == 0 ||
        strcmp(subfield, "off_threshold") == 0) {
        int v;
        if (strcmp(value, "Off") == 0 || strcmp(value, "off") == 0 ||
            strcmp(value, "0") == 0) {
            v = 0;
        } else {
            v = clamp_int(value, 15, 50, &ok);
            if (!ok) { cJSON_Delete(block); return NULL; }
        }
        set_num(block, strcmp(subfield, "dim_threshold") == 0 ? "darkTemp"
                                                              : "offTemp", v);

    } else if (strcmp(subfield, "schedule_brightness") == 0) {
        // The app's target brightness runs 11-100%. The controller's
        // floor is 1 and it is never asked for 0 by the app, but 11 is
        // offered because a quarter of a percent is not a light level.
        int v = clamp_int(value, 11, 100, &ok);
        if (!ok) { cJSON_Delete(block); return NULL; }
        set_num(period_0(block, "timePeriod"), "brightness", v);

    } else if (strcmp(subfield, "schedule_start") == 0) {
        set_num(period_0(block, "timePeriod"), "startTime", sf_hhmm_to_seconds(value));

    } else if (strcmp(subfield, "schedule_end") == 0) {
        set_num(period_0(block, "timePeriod"), "endTime", sf_hhmm_to_seconds(value));

    } else if (strcmp(subfield, "fade_minutes") == 0) {
        // Sunrise and sunset simulation, in minutes: Off, or 1-60.
        int v;
        if (strcmp(value, "Off") == 0 || strcmp(value, "off") == 0 ||
            strcmp(value, "0") == 0) {
            v = 0;
        } else {
            v = clamp_int(value, 1, 60, &ok);
            if (!ok) { cJSON_Delete(block); return NULL; }
        }
        set_num(period_0(block, "timePeriod"), "fadeTime", v * 60);

    } else if (strcmp(subfield, "ppfd_target") == 0) {
        // In micromoles, over the app's own 20-2000. The earlier 0-1000
        // cap came from the sensor's reporting scale, not from what the
        // controller will accept: a target above 1000 is a light that is
        // meant to run brighter than the sensor reads, which is the
        // point of a target in PPFD mode.
        int v = clamp_int(value, 20, 2000, &ok);
        if (!ok) { cJSON_Delete(block); return NULL; }
        set_num(period_0(block, "ppfdPeriod"), "brightness", v);

    } else if (strcmp(subfield, "ppfd_start") == 0) {
        set_num(period_0(block, "ppfdPeriod"), "startTime", sf_hhmm_to_seconds(value));

    } else if (strcmp(subfield, "ppfd_end") == 0) {
        set_num(period_0(block, "ppfdPeriod"), "endTime", sf_hhmm_to_seconds(value));

    } else if (strcmp(subfield, "ppfd_fade_minutes") == 0) {
        // The PPFD ramp, over the same 1-60 minutes as the brightness
        // fade above: the controller stores both in seconds and the app
        // offers neither beyond an hour.
        int v = clamp_int(value, 1, 60, &ok);
        if (!ok) { cJSON_Delete(block); return NULL; }
        set_num(period_0(block, "ppfdPeriod"), "fadeTime", v * 60);

    // The PPFD dimming band, as brightness percents. Both ends run 11-100 in
    // the app, and both are the same two controller fields as the target
    // brightness above -- only read at a different point in the ramp.
    } else if (strcmp(subfield, "ppfd_min") == 0 ||
               strcmp(subfield, "ppfd_max") == 0) {
        int v = clamp_int(value, 11, 100, &ok);
        if (!ok) { cJSON_Delete(block); return NULL; }
        set_num(block, strcmp(subfield, "ppfd_min") == 0 ? "ppfdMinBrightness"
                                                        : "ppfdMaxBrightness", v);

    } else {
        cJSON_Delete(block);
        return NULL;
    }

    return build_command(mac, uid, "device", field, block);
}

// ---------------------------------------------------------------------------
// Light on/off, brightness and mode
// ---------------------------------------------------------------------------
static cJSON *handle_light(const char *field, const char *value,
                           const char *mac, const char *uid)
{
    // Home Assistant sends either a bare state string or a JSON object
    // with state, brightness and effect.
    cJSON *cmd = cJSON_Parse(value);
    bool parsed = cJSON_IsObject(cmd);
    if (!parsed) {
        if (cmd) cJSON_Delete(cmd);
        cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(cmd, "state", value);
    }

    device_cache_lock();
    cJSON *cached = device_cache_get(mac, field);
    int cur_on = 0, cur_level = 0, cur_last_auto = 0;
    cJSON *cur_tp = NULL;
    if (cached) {
        cJSON *v = cJSON_GetObjectItem(cached, "on");
        if (!v) v = cJSON_GetObjectItem(cached, "mOnOff");
        cur_on = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
        v = cJSON_GetObjectItem(cached, "level");
        if (!v) v = cJSON_GetObjectItem(cached, "mLevel");
        cur_level = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
        v = cJSON_GetObjectItem(cached, "lastAutoModeType");
        cur_last_auto = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
        v = cJSON_GetObjectItem(cached, "timePeriod");
        if (cJSON_IsArray(v)) cur_tp = cJSON_Duplicate(v, true);
    }
    device_cache_unlock();

    cJSON *state = cJSON_GetObjectItem(cmd, "state");
    int on = onoff_val(cJSON_IsString(state) ? state->valuestring : "ON");

    cJSON *effect = cJSON_GetObjectItem(cmd, "effect");
    cJSON *brightness = cJSON_GetObjectItem(cmd, "brightness");

    // Mode-only change while the light is off.
    //
    // Home Assistant's light.turn_on always implies state=ON, so picking
    // a mode from the dropdown would briefly light the lamp before the
    // schedule turns it back off — a visible flash. When only an effect
    // was sent and the light is currently off, keep it off and change
    // just the mode.
    if (effect && !brightness && cur_on == 0) {
        on = 0;
    }

    int level;
    if (cJSON_IsNumber(brightness)) {
        level = (int)brightness->valuedouble;
    } else {
        level = cur_level;
        // The controller reports level 0 while the light is off, so
        // restore the last non-zero brightness on OFF -> ON.
        if (on == 1 && level == 0) {
            level = device_cache_last_level(mac, field, 100);
        }
    }
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    int mode = 0;
    if (cJSON_IsString(effect)) {
        int m = lookup_mode(effect->valuestring, LIGHT_MODES, LIGHT_MODE_COUNT);
        if (m >= 0) mode = m;
    }

    cJSON_Delete(cmd);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "modeType", mode);
    cJSON_AddNumberToObject(obj, "lastAutoModeType", cur_last_auto);
    cJSON_AddNumberToObject(obj, "mOnOff", on);
    cJSON_AddNumberToObject(obj, "mLevel", level);
    cJSON_AddItemToObject(obj, "timePeriod",
                          cur_tp ? cur_tp : default_time_period());

    return build_command(mac, uid, "device", field, obj);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
// Works out the POSIX rules a controller should be running, given the
// bridge's zone and daylight-saving choice.
//
// Shared by the push and the check, so the two can never disagree about
// what "correct" means — which would make the check fire endlessly.
// Splits a POSIX TZ string into its standard-time name, its UTC offset
// in minutes (converted to the usual east-positive sense; POSIX itself
// uses the opposite sign), and the name of the summer-time abbreviation
// if one follows — e.g. "CET-1CEST" gives name="CET", east=+60,
// dst_name="CEST".
//
// Replaces a hand-rolled character scanner that read past the end of a
// malformed or unexpected string under some inputs — the crash this was
// written to fix. Every index here is bounded against the string length
// before it is used, which the previous version did not do consistently.
static bool parse_tz_std(const char *tz, char *name, size_t name_sz,
                         int *east_minutes, char *dst_name, size_t dst_sz)
{
    size_t len = strlen(tz);
    size_t i = 0, n = 0;

    while (i < len && n + 1 < name_sz &&
           !(tz[i] == '-' || tz[i] == '+' || (tz[i] >= '0' && tz[i] <= '9'))) {
        name[n++] = tz[i++];
    }
    name[n] = '\0';
    if (n == 0) return false;   // no name at all: not a usable TZ string

    int sign = 1;
    if (i < len && tz[i] == '-') { sign = -1; i++; }
    else if (i < len && tz[i] == '+') { i++; }

    int hours = 0;
    bool saw_digit = false;
    while (i < len && tz[i] >= '0' && tz[i] <= '9') {
        hours = hours * 10 + (tz[i++] - '0');
        saw_digit = true;
    }
    if (!saw_digit) return false;   // name with no offset: can't use it here

    int mins = 0;
    if (i < len && tz[i] == ':') {
        i++;
        while (i < len && tz[i] >= '0' && tz[i] <= '9') {
            mins = mins * 10 + (tz[i++] - '0');
        }
    }

    *east_minutes = -sign * (hours * 60 + mins);

    // Whatever letters remain, up to the switching rules, are the
    // summer-time abbreviation. Absent for a fixed-offset zone like
    // "UTC0", which is a normal case, not an error.
    if (dst_name && dst_sz > 0) {
        size_t k = 0;
        while (i < len && tz[i] != ',' && k + 1 < dst_sz) {
            dst_name[k++] = tz[i++];
        }
        dst_name[k] = '\0';
    }

    return true;
}

static void wanted_posix(const sb_prov_cfg_t *prov, char *out, size_t out_sz)
{
    char posix[56];
    strncpy(posix, prov->tz[0] ? prov->tz : "CET-1CEST,M3.5.0,M10.5.0/3",
            sizeof(posix) - 1);
    posix[sizeof(posix) - 1] = '\0';

    if (prov->dst_mode != SB_DST_AUTO) {
        // Forcing standard or summer time means stripping the switching
        // rules: a TZ string that still carries them would switch back
        // at the next changeover and quietly undo the choice.
        char *comma = strchr(posix, ',');
        if (comma) *comma = '\0';

        if (prov->dst_mode == SB_DST_SUMMER) {
            char std_name[8] = "", dst_name[8] = "";
            int east_min = 0;

            if (parse_tz_std(posix, std_name, sizeof(std_name), &east_min,
                             dst_name, sizeof(dst_name))) {
                // One hour further east while saving is in force. Prefer
                // the zone's own summer abbreviation (CEST, BST, ...)
                // when it has one; a made-up name would be more
                // confusing than reusing the standard one.
                const char *use_name = dst_name[0] ? dst_name : std_name;

                int east_total = east_min + 60;

                // Back to the POSIX sign convention, which is the
                // opposite of the usual "east is positive" — the
                // conversion parse_tz_std() undoes on the way in.
                // Verified against a real device reply: CET (east +60
                // normally, +120 forced) must come out as "CEST-2:00".
                int posix_val = -east_total;
                int at = posix_val < 0 ? -posix_val : posix_val;
                snprintf(posix, sizeof(posix), "%s%s%d:%02d", use_name,
                         posix_val < 0 ? "-" : "+", at / 60, at % 60);
            }
            // If parsing fails, posix already holds the rule-stripped
            // standard-time string from above, which is at least valid.
        }
    }

    strncpy(out, posix, out_sz - 1);
    out[out_sz - 1] = '\0';
}

void sf_timezone_check(const char *mac, const char *their_name,
                       const char *their_posix)
{
    if (!prov_tz_push()) return;
    if (!mac || !mac[0] || !their_posix) return;

    // On the heap: ~1 KB, and sf_push_timezone() below needs another one,
    // on the 6 KB MQTT task right before an mbedTLS write (a stack copy of
    // this struct crashed that task once already).
    sb_prov_cfg_t *prov = malloc(sizeof(*prov));
    if (!prov) return;
    sb_prov_load(prov);

    char want[56];
    wanted_posix(prov, want, sizeof(want));

    char want_name[40];
    snprintf(want_name, sizeof(want_name), "%s", prov->tz_name[0] ? prov->tz_name : "Europe/Berlin");
    free(prov);

    // The rules decide the clock, so a difference there always matters.
    // A differing label with identical rules is cosmetic, but still
    // worth correcting so the app shows the zone that was chosen.
    bool rules_differ = strcmp(their_posix, want) != 0;
    bool name_differs = their_name && their_name[0] &&
                        strcmp(their_name, want_name) != 0;

    if (!rules_differ && !name_differs) return;

    ESP_LOGI(TAG, "%s reports %s (%s), correcting to %s (%s)",
             mac, their_name ? their_name : "?", their_posix,
             want_name, want);

    sf_push_timezone(mac, mitm_proxy_uid_for(mac));
}

// "Sync device time", as in the app: setDevTimezone with the bridge's zone
// and its current UTC clock. Sent regardless of the "apply to every
// controller" setting -- it is an explicit request. With the bridge's own
// zone setting when that is in force, otherwise the zone the controller
// last reported, so a manual sync never changes the zone.
bool sf_sync_device_time(const char *mac, const char *uid)
{
    if (!mac || !mac[0]) return false;
    if (time(NULL) < 1600000000) return false;      // bridge clock not set
    sb_prov_cfg_t *prov = malloc(sizeof(*prov));
    if (!prov) return false;
    sb_prov_load(prov);
    char name[40] = "", posix[56] = "";
    if (prov->tz_push) {
        wanted_posix(prov, posix, sizeof(posix));
        strncpy(name, prov->tz_name[0] ? prov->tz_name : "Europe/Berlin", sizeof(name) - 1);
    } else {
        device_registry_lock();
        device_entry_t *d = device_registry_find(mac);
        if (d && d->tz_known) {
            strncpy(name, d->tz_name, sizeof(name) - 1);
            strncpy(posix, d->tz_posix, sizeof(posix) - 1);
        }
        device_registry_unlock();
        if (!posix[0]) {
            wanted_posix(prov, posix, sizeof(posix));
            strncpy(name, prov->tz_name[0] ? prov->tz_name : "Europe/Berlin", sizeof(name) - 1);
        }
    }
    free(prov);
    char value[100];
    snprintf(value, sizeof(value), "%s|%s", name, posix);
    cJSON *cmd = sf_translate_command("timezone", NULL, value, mac, uid ? uid : "");
    if (!cmd) return false;
    char *json = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    if (!json) return false;
    bool ok = mitm_proxy_inject_command_to(mac, json, strlen(json));
    cJSON_free(json);
    ESP_LOGI(TAG, "Device time synced for %s (%s, %s)", mac, name, posix);
    return ok;
}

void sf_push_timezone(const char *mac, const char *uid)
{
    if (!mac || !mac[0]) return;
    sb_prov_cfg_t *prov = malloc(sizeof(*prov));   // heap: see sf_timezone_check
    if (!prov) return;
    sb_prov_load(prov);
    if (!prov->tz_push) { free(prov); return; }

    char posix[56];
    wanted_posix(prov, posix, sizeof(posix));

    char tzname[40];
    snprintf(tzname, sizeof(tzname), "%s", prov->tz_name[0] ? prov->tz_name : "Europe/Berlin");
    free(prov);

    char value[100];
    snprintf(value, sizeof(value), "%s|%s", tzname, posix);

    cJSON *cmd = sf_translate_command("timezone", NULL, value, mac,
                                      uid ? uid : "");
    if (!cmd) return;

    char *json = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    if (!json) return;

    mitm_proxy_inject_command_to(mac, json, strlen(json));
    ESP_LOGI(TAG, "Sent the bridge time zone to %s: %s (%s)",
             mac, tzname, posix);
    cJSON_free(json);
}

void sf_push_timezone_all(void)
{
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        device_registry_lock();
        device_entry_t *d = device_registry_at(i);
        char mac[DEV_MAC_LEN] = "";
        char their_name[32] = "", their_posix[48] = "";
        bool on = false, known = false;
        if (d) {
            strncpy(mac, d->mac, sizeof(mac) - 1);
            strncpy(their_name, d->tz_name, sizeof(their_name) - 1);
            strncpy(their_posix, d->tz_posix, sizeof(their_posix) - 1);
            on = d->online;
            known = d->tz_known;
        }
        device_registry_unlock();

        if (!mac[0] || !on) continue;

        // Only when it differs. Sending regardless means a command
        // injected into a live session on a timer, which the cloud
        // treats as a reason to close the connection.
        if (known) {
            sf_timezone_check(mac, their_name, their_posix);
        } else {
            // Nothing reported yet, so there is nothing to compare —
            // send once to establish the setting.
            sf_push_timezone(mac, mitm_proxy_uid_for(mac));
        }
    }
}

// Re-sends the bridge's clock settings, but only when they are wrong.
//
// An earlier version sent them every thirty seconds regardless. That is
// a command injected into a live session twice a minute, and the cloud
// closes the connection over it — the observed symptom was a controller
// that worked briefly after boot and then dropped offline, which looked
// like a calibration bug because setting calibration was when people
// noticed.
//
// Nothing is sent now unless a mismatch is actually seen. The status
// path calls sf_timezone_check() on every system frame, so a change made
// from the vendor app is still corrected within seconds — by one
// command, once, rather than a standing stream of them.
//
// This slow loop remains only as a backstop for a controller that stops
// reporting its zone. An hour is long enough to be invisible in the
// traffic and short enough to matter.
// Runs on the config-poll task's hook (no task of its own).
//
// With "Apply this to every controller" on, each connected controller's
// clock is kept in step with the bridge: a full time sync (zone and UTC,
// one setDevTimezone) every six hours -- the same command the app's "Sync
// device time" sends. On connect the same happens (see mitm_proxy). Six
// hours keeps drift invisible while staying far from the injection rate
// that once made the cloud close sessions (every 30 s).
static void tz_sync_hook(void)
{
    if (!prov_tz_push()) return;
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        device_registry_lock();
        device_entry_t *d = device_registry_at(i);
        char mac[DEV_MAC_LEN] = "";
        bool on = false;
        if (d) { strncpy(mac, d->mac, sizeof(mac) - 1); on = d->online; }
        device_registry_unlock();
        if (mac[0] && on) sf_sync_device_time(mac, mitm_proxy_uid_for(mac));
    }
}

void sf_plan_window_start(void);

void sf_timezone_sync_start(void)
{
    sf_plan_window_start();
    config_poll_add_hook(tz_sync_hook, 6 * 3600);
}

cJSON *sf_translate_command(const char *field, const char *subfield,
                            const char *value, const char *mac, const char *uid)
{
    if (!field || !value || !mac) return NULL;
    s_note[0] = '\0';

    // --- Outlets ---
    // Minimal payload on purpose: the controller already holds the
    // schedule state, we only flip mOnOff and force manual mode.
    if (strncmp(field, "outlet_", 7) == 0) {
        int n = atoi(field + 7);
        if (n < 1 || n > 10) {
            ESP_LOGW(TAG, "Out-of-range outlet number: %s", field);
            return NULL;
        }
        char key[8];
        snprintf(key, sizeof(key), "O%d", n);
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "modeType", 0);
        cJSON_AddNumberToObject(obj, "mOnOff", onoff_val(value));
        return build_command(mac, uid, "outlet", key, obj);
    }

    // --- Sensor calibration ---
    //
    // Offsets the controller applies to its own readings, so the
    // corrected values reach the app and the vendor cloud too — not just
    // Home Assistant.
    //
    // The wire format was captured from the vendor app rather than
    // guessed: keyPath ["calibration"] with no "device" prefix, and
    // plain field names. An earlier attempt using ["device","sensor"]
    // and tempOffset-style names was silently ignored, which is this
    // controller's only way of saying no.
    //
    // All four fields go in every command. A partial block is discarded,
    // so the three not being changed are sent at their cached values.
    if (strcmp(field, "calibration") == 0 && subfield) {
        // Rapid-fire calibration changes have been reproduced to crash
        // the device hard enough to need a power cycle. Refusing to
        // build the command at all, this early, keeps the controller
        // from being flooded regardless of what else calls into this —
        // see mitm_proxy_calibration_throttled().
        if (mitm_proxy_calibration_throttled(mac)) {
            ESP_LOGW(TAG, "Calibration change for %s ignored — sent too "
                          "soon after the previous one", mac);
            return NULL;
        }

        static const char *CAL_FIELDS[] = { "temp", "humi", "co2", "ppfd" };
        bool known = false;
        for (int i = 0; i < 4; i++) {
            if (strcmp(subfield, CAL_FIELDS[i]) == 0) { known = true; break; }
        }
        if (!known) {
            ESP_LOGW(TAG, "Unknown calibration field '%s'", subfield);
            return NULL;
        }

        cJSON *obj = cJSON_CreateObject();

        // Ranges and steps, as the app offers them: temperature and
        // humidity in tenths, CO2 in tens of ppm, the PPFD offset in
        // tenths. Enforced here rather than only in the interface because
        // the command topic is open to anything that publishes to it.
        static const struct {
            double lo, hi, step;
        } CAL_RANGE[] = {
            { -10.0,  10.0, 0.1 },   // temp
            { -20.0,  20.0, 0.1 },   // humi
            { -200.0, 200.0, 10.0 },  // co2
            { -20.0,  20.0, 0.1 },   // ppfd
        };

        device_cache_lock();
        cJSON *cached = device_cache_get(mac, "calibration");
        for (int i = 0; i < 4; i++) {
            double v = 0.0;
            if (strcmp(subfield, CAL_FIELDS[i]) == 0) {
                bool ok = true;
                v = clamp_double(value, CAL_RANGE[i].lo, CAL_RANGE[i].hi, &ok);
                if (!ok) {
                    ESP_LOGW(TAG, "calibration/%s: '%s' is not a number", subfield, value);
                    device_cache_unlock();
                    cJSON_Delete(obj);
                    return NULL;
                }
                // Snapped to the step, so a topic carrying an arbitrary
                // float cannot write 0.37 to a controller whose own
                // controls move in tenths. Snapping to nearest rather than
                // truncating keeps -0.06 from becoming -0.1 and 0.06
                // becoming 0.0.
                double st = CAL_RANGE[i].step;
                v = (v >= 0) ? (floor(v / st + 0.5) * st)
                              : (ceil(v / st - 0.5) * st);
            } else if (cached) {
                cJSON *c = cJSON_GetObjectItem(cached, CAL_FIELDS[i]);
                if (cJSON_IsNumber(c)) v = c->valuedouble;
            }
            cJSON_AddNumberToObject(obj, CAL_FIELDS[i], v);
        }
        device_cache_unlock();

        return build_command_flat(mac, uid, "calibration", obj);
    }

    // --- Day cycle targets ---
    //
    // Same shape as calibration and for the same reason: keyPath
    // ["target"] with no "device" prefix, and the controller discards a
    // partial block. So changing one target means sending all twelve
    // values, and the eleven not being changed come from the cache.
    //
    // Sending a block assembled without that cache would reset every other
    // target to zero, which for a running tent means the climate control
    // suddenly has no setpoints at all. So the cached block is required:
    // without it the command is refused rather than sent incomplete.
    if (strcmp(field, "target") == 0 && subfield) {
        // Typed as a named struct, not an anonymous one: the table is
        // declared with an anonymous struct type and taking a pointer to
        // an element of it through a separately spelled-out type is
        // incompatible even when the members match.
        typedef struct {
            const char *sub;
            const char *group;     // NULL for the day window
            const char *key;
            bool        is_time;   // HH:MM rather than a number
            double      min, max;
        } target_field_t;

        // subfield -> (group, key). The two times of day live in a group
        // of their own with a start/end rather than day/night shape.
        static const target_field_t TARGET_FIELDS[] = {
            { "day_time_start",   NULL,     "startTime",   true,  0, 86399 },
            { "day_time_end",     NULL,     "endTime",     true,  0, 86399 },
            { "temp_target_day",  "temp",   "targetDay",   false, 0, 50 },
            { "temp_target_night","temp",   "targetNight", false, 0, 50 },
            { "temp_deadband",    "temp",   "deadband",    false, 1, 10 },
            { "humi_target_day",  "humi",   "targetDay",   false, 0, 100 },
            { "humi_target_night","humi",   "targetNight", false, 0, 100 },
            { "humi_deadband",    "humi",   "deadband",    false, 1, 10 },
            { "co2_target_day",   "co2",    "targetDay",   false, 0, 2500 },
            { "co2_target_night", "co2",    "targetNight", false, 0, 2500 },
            { "co2_deadband",     "co2",    "deadband",    false, 10, 250 },
        };
        const size_t N_TARGET =
            sizeof(TARGET_FIELDS) / sizeof(TARGET_FIELDS[0]);

        size_t idx = N_TARGET;
        for (size_t i = 0; i < N_TARGET; i++) {
            if (strcmp(subfield, TARGET_FIELDS[i].sub) == 0) { idx = i; break; }
        }
        if (idx == N_TARGET) {
            ESP_LOGW(TAG, "Unknown target field '%s'", subfield);
            return NULL;
        }
        const target_field_t *f = &TARGET_FIELDS[idx];

        device_cache_lock();
        cJSON *cached = device_cache_get(mac, "target");
        cJSON *obj = cached ? cJSON_Duplicate(cached, true) : NULL;
        device_cache_unlock();

        // Every value has to be present before this is safe to send.
        //
        // The two day window entries have no group, so they are looked up
        // under "dayTime" rather than skipped. Skipping them looked correct
        // -- they are checked separately below -- but the loop that walks
        // the table for all twelve was also skipping them, and with the
        // group pointer NULL it asked cJSON for the empty name, found
        // nothing, and reported every target change as incomplete.
        bool complete = obj != NULL;
        for (size_t i = 0; complete && i < N_TARGET; i++) {
            const char *group = TARGET_FIELDS[i].group
                              ? TARGET_FIELDS[i].group : "dayTime";
            cJSON *grp = cJSON_GetObjectItem(obj, group);
            cJSON *v = cJSON_IsObject(grp)
                     ? cJSON_GetObjectItem(grp, TARGET_FIELDS[i].key) : NULL;
            if (!cJSON_IsNumber(v)) complete = false;
        }
        if (!complete) {
            cJSON_Delete(obj);
            ESP_LOGW(TAG, "Target change for '%s' not sent: the cached "
                          "day cycle block is incomplete, and sending a "
                          "partial one would reset the other targets to 0",
                     subfield);
            return NULL;
        }

        // Validate before writing, so an out-of-range value from a stale
        // or hand-edited payload is refused rather than clamped into
        // something the user did not ask for.
        bool ok = true;
        double v;
        cJSON *grp = f->group ? cJSON_GetObjectItem(obj, f->group) : NULL;
        if (f->is_time) {
            v = sf_hhmm_to_seconds(value);
        } else {
            v = clamp_double(value, f->min, f->max, &ok);
        }
        if (!ok) {
            cJSON_Delete(obj);
            return NULL;
        }

        if (f->is_time) {
            cJSON *dt = cJSON_GetObjectItem(obj, "dayTime");
            set_num(dt, f->key, (int)v);
        } else {
            set_num(grp, f->key, v);
        }

        return build_command_flat(mac, uid, "target", obj);
    }

    // --- Alarm settings ---
    //
    // keyPath ["alarm"], sent whole like calibration and target: the
    // controller replaces the block, so every field not being changed is
    // taken from the cache, and without a cached block nothing is sent.
    //
    // Topics:  alarm/<range>/set  with subfield enabled|min|max
    //          alarm/<switch>/set
    // The command topic is spiderfarmer/<slug>/command/alarm/<a>/<b>/set,
    // which arrives here as field "alarm", subfield "<a>" and -- for the
    // range alarms -- the part after it in value as "<b>=<v>"; see the
    // caller in ha_mqtt.c / control_page.c, which joins them that way.
    if (strcmp(field, "alarm") == 0 && subfield) {
        device_cache_lock();
        cJSON *cached = device_cache_get(mac, "alarm");
        cJSON *obj = cached ? cJSON_Duplicate(cached, true) : NULL;
        device_cache_unlock();
        if (!obj) {
            ESP_LOGW(TAG, "%s: alarm settings not read yet -- not sending a "
                          "partial block", mac);
            return NULL;
        }

        // Range alarm: "temp", value "enabled=ON" / "min=16" / "max=32"
        const alarm_range_t *rg = NULL;
        for (int i = 0; i < ALARM_RANGE_COUNT; i++) {
            if (strcmp(ALARM_RANGES[i].key, subfield) == 0) { rg = &ALARM_RANGES[i]; break; }
        }
        if (rg) {
            const char *eq = strchr(value, '=');
            if (!eq) { cJSON_Delete(obj); return NULL; }
            char what[12] = "";
            size_t wl = (size_t)(eq - value);
            if (wl >= sizeof(what)) { cJSON_Delete(obj); return NULL; }
            memcpy(what, value, wl);
            const char *v = eq + 1;

            cJSON *g = cJSON_GetObjectItem(obj, rg->key);
            if (!cJSON_IsObject(g)) {
                g = cJSON_CreateObject();
                cJSON_AddItemToObject(obj, rg->key, g);
            }
            if (strcmp(what, "enabled") == 0) {
                // "enabled":1 or "enabled":0. The app leaves the field out
                // for alarms it never touched, but an explicit 0 is what it
                // sends when switching one off, and what the controller and
                // the cloud both accept (confirmed with a captured command).
                set_num(g, "enabled", onoff_val(v) ? 1 : 0);
            } else if (strcmp(what, "min") == 0 || strcmp(what, "max") == 0) {
                if (strcmp(what, "min") == 0 && !rg->has_min) {
                    cJSON_Delete(obj); return NULL;
                }
                bool is_min = strcmp(what, "min") == 0;
                bool ok = true;
                double d = clamp_double(v, is_min ? rg->min_lo : rg->max_lo,
                                        is_min ? rg->min_hi : rg->max_hi, &ok);
                if (!ok) { cJSON_Delete(obj); return NULL; }
                // Snapped to the step, and rounded to one decimal so 1.7
                // goes out as 1.7 rather than 1.7000000000000002.
                d = round(d / rg->step) * rg->step;
                d = round(d * 10.0) / 10.0;
                // min never above max and the other way round.
                cJSON *other = cJSON_GetObjectItem(g, strcmp(what, "min") == 0 ? "vmax" : "vmin");
                if (cJSON_IsNumber(other)) {
                    if (strcmp(what, "min") == 0 && d > other->valuedouble) d = other->valuedouble;
                    if (strcmp(what, "max") == 0 && d < other->valuedouble) d = other->valuedouble;
                }
                set_num(g, strcmp(what, "min") == 0 ? "vmin" : "vmax", d);
            } else {
                cJSON_Delete(obj); return NULL;
            }
            return build_command_flat(mac, uid, "alarm", obj);
        }

        // Plain switch: "devOffline", value ON/OFF
        for (int i = 0; i < ALARM_SWITCH_COUNT; i++) {
            if (strcmp(ALARM_SWITCHES[i].key, subfield) == 0) {
                set_num(obj, subfield, onoff_val(value));
                return build_command_flat(mac, uid, "alarm", obj);
            }
        }
        cJSON_Delete(obj);
        return NULL;
    }

    // --- Time zone ---
    //
    // A different method from everything else: setDevTimezone, with the
    // parameters at the top of params rather than under a keyPath.
    // Captured from the vendor app:
    //
    //   {"method":"setDevTimezone","params":{"timezone":"Europe/Berlin",
    //    "TZ":"CET-1CEST,M3.5.0,M10.5.0/3","UTC":...,"gmtoff":0},...}
    //
    // The TZ string is what actually decides daylight saving. A rule
    // form like "CET-1CEST,M3.5.0,M10.5.0/3" switches on its own; a
    // plain "CET-1" pins the clock to standard time year round. That is
    // how summer time is turned off — there is no separate flag.
    // --- Per-device daylight saving ---
    //
    // "dst" changes only the switching behaviour of whatever zone the
    // controller currently reports, leaving the zone itself untouched.
    // Separate from "timezone" below, which replaces both at once.
    //
    // value is "0" automatic, "1" standard, "2" summer — matching
    // SB_DST_AUTO/STANDARD/SUMMER. The zone this works from is the one
    // device_registry_set_tz() last recorded from getSysSta; without a
    // known zone there is nothing to apply a switching rule to.
    if (strcmp(field, "dst") == 0) {
        char their_name[32] = "", their_posix[48] = "";
        bool known = false;
        device_registry_lock();
        device_entry_t *d = device_registry_find(mac);
        if (d && d->tz_known) {
            strncpy(their_name, d->tz_name, sizeof(their_name) - 1);
            strncpy(their_posix, d->tz_posix, sizeof(their_posix) - 1);
            known = true;
        }
        device_registry_unlock();

        if (!known) {
            ESP_LOGW(TAG, "%s has not reported a time zone yet — cannot "
                          "change daylight saving", mac);
            return NULL;
        }

        int mode = atoi(value);
        if (mode < SB_DST_AUTO || mode > SB_DST_SUMMER) return NULL;

        // On the heap and zero-initialised.
        //
        // The crash this fixes came from the stack version of this: only
        // tz and dst_mode were set, and wanted_posix() is passed the
        // whole struct by pointer — nothing stopped a later change to
        // that function from reading one of the other fields, which
        // would then be whatever garbage happened to be on the stack.
        // calloc() costs one allocation on a path that already runs
        // rarely (a user clicking a DST switch), and removes the
        // uninitialised-read class of bug entirely rather than
        // depending on every future reader of the struct being careful.
        sb_prov_cfg_t *local_prov = calloc(1, sizeof(*local_prov));
        if (!local_prov) return NULL;

        strncpy(local_prov->tz, their_posix, sizeof(local_prov->tz) - 1);
        local_prov->dst_mode = mode;

        char posix[56];
        wanted_posix(local_prov, posix, sizeof(posix));
        free(local_prov);

        char tz_value[96];
        snprintf(tz_value, sizeof(tz_value), "%s|%s", their_name, posix);

        return sf_translate_command("timezone", NULL, tz_value, mac, uid);
    }

    if (strcmp(field, "timezone") == 0) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "method", "setDevTimezone");

        cJSON *params = cJSON_CreateObject();

        // value is "Name|TZ", or just a TZ string.
        char name[40] = "", posix[56] = "";
        const char *bar = strchr(value, '|');
        if (bar) {
            size_t n = (size_t)(bar - value);
            if (n >= sizeof(name)) n = sizeof(name) - 1;
            memcpy(name, value, n);
            name[n] = '\0';
            strncpy(posix, bar + 1, sizeof(posix) - 1);
        } else {
            strncpy(posix, value, sizeof(posix) - 1);
        }

        cJSON_AddStringToObject(params, "timezone",
                                name[0] ? name : "Europe/Berlin");
        cJSON_AddStringToObject(params, "TZ", posix);
        cJSON_AddNumberToObject(params, "UTC", (double)time(NULL));
        cJSON_AddNumberToObject(params, "gmtoff", 0);
        cJSON_AddItemToObject(root, "params", params);

        char msg_id[32];
        make_msg_id(msg_id, sizeof(msg_id));
        cJSON_AddStringToObject(root, "msgId", msg_id);
        cJSON_AddStringToObject(root, "pid", mac);
        cJSON_AddStringToObject(root, "uid", uid);
        return root;
    }

    // --- Sensor cleaning ---
    //
    // Not a config write, so it sits outside the keyPath machinery above
    // and goes to the controller as setSensorHeating. The controller owns
    // the two-hour cycle and the five-minute cooldown afterwards; both are
    // reported back through getDevStatus rather than tracked here, so the
    // bridge never has to guess when a cycle has finished.
    // "sensor_heating" is the field's earlier name, still accepted so
    // automations written against it keep working.
    if (strcmp(field, "sensor_cleaning") == 0 ||
        strcmp(field, "sensor_heating") == 0) {
        int on;
        if (strcasecmp(value, "ON") == 0 || strcmp(value, "1") == 0) {
            on = 1;
        } else if (strcasecmp(value, "OFF") == 0 || strcmp(value, "0") == 0) {
            on = 0;
    } else {
        ESP_LOGW(TAG, "sensor_cleaning: '%s' is neither ON nor OFF", value);
            return NULL;
        }
        ESP_LOGI(TAG, "Sensor cleaning for %s: %s", mac, on ? "start" : "stop");

        // Recorded before sending, so the switch state survives a frame
        // that does not carry the field. The controller owns the two-hour
        // timer and the cooldown, so nothing is scheduled here.
        // Anticipates the controller's own report, which follows within a
        // few seconds: on starts the two-hour cleaning, off from a running
        // cycle starts the five-minute cooldown. The next getDevSta
        // corrects both the phase and the exact remaining time.
        int phase = 0;
        bool reported = device_cache_sensor_cleaning_phase(mac, &phase, NULL);
        if (on) {
            device_cache_set_sensor_cleaning_phase(mac, 1, 7200);
        } else if (reported && phase == 1) {
            device_cache_set_sensor_cleaning_phase(mac, 2, 300);
        } else {
            device_cache_set_sensor_cleaning(mac, false);
        }
        return build_command_heating(mac, uid, on);
    }

    // --- Pairing ---
    //
    // "pairing" OFF = setDevDeactive: the controller drops its binding to
    // the account, advertises over Bluetooth again and a phone can pair
    // with it. ON = setDevActive, the reverse (captured from the app:
    // {"method":"setDevDeactive","params":{"uid":"162606"},...}). The
    // Wi-Fi settings are not touched by either.
    if (strcmp(field, "pairing") == 0 || strcmp(field, "unpair") == 0) {
        bool off = strcmp(field, "unpair") == 0 || strcasecmp(value, "OFF") == 0 ||
                   strcmp(value, "0") == 0 || strcasecmp(value, "PRESS") == 0;
        // Binding needs an account uid. An unbound controller reports none
        // in its session, so the one it last reported is used.
        const char *use = (uid && uid[0]) ? uid : device_registry_uid(mac);
        if (!off && !use[0]) {
            ESP_LOGW(TAG, "Pairing %s: no account uid known -- pair it once with the app first", mac);
            return NULL;
        }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "method", off ? "setDevDeactive" : "setDevActive");
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "uid", use);
        if (!off) {
            // The app sends the account's user name alongside; it is
            // "devd_<uid>" for every account seen so far (prefix "test_"
            // on the vendor's test server).
            char uname[40];
            snprintf(uname, sizeof(uname), "devd_%s", use);
            cJSON_AddStringToObject(params, "uname", uname);
        }
        cJSON_AddItemToObject(root, "params", params);
        char msg_id[32];
        make_msg_id(msg_id, sizeof(msg_id));
        cJSON_AddStringToObject(root, "msgId", msg_id);
        cJSON_AddStringToObject(root, "pid", mac);
        cJSON_AddStringToObject(root, "uid", use);
        ESP_LOGI(TAG, "%s %s with account %s (%s)", off ? "Unpairing" : "Pairing", mac, use,
                 off ? "it advertises over Bluetooth again" : "Bluetooth advertising stops");
        return root;
    }

    bool is_light = (strcmp(field, "light") == 0 || strcmp(field, "light2") == 0);
    bool is_fan   = (strcmp(field, "fan") == 0 || strcmp(field, "blower") == 0);

    // --- Subfield writes ---
    if (subfield && is_light) {
        return handle_light_subfield(field, subfield, value, mac, uid);
    }
    if (subfield && is_fan && strcmp(subfield, "percentage") != 0) {
        return handle_fan_subfield(field, subfield, value, mac, uid);
    }

    // --- Light on/off, brightness, mode ---
    if (is_light) {
        return handle_light(field, value, mac, uid);
    }

    // --- Fan and blower ---
    if (is_fan) {
        // The blower accepts only 25-100; see the fan block handler above.
        int speed_min = (strcmp(field, "blower") == 0) ? 25 : 1;
        int speed_max = (strcmp(field, "blower") == 0) ? 100 : 10;

        // Start from the cached block instead of an empty object.
        //
        // This path runs for every on/off and every speed change. It
        // used to build a fresh object holding only mOnOff, mLevel,
        // shakeLevel, natural and a stub timePeriod — so modeType,
        // maxSpeed, minSpeed and cycleTime were absent from the command
        // and the controller lost them. Turning the fan down therefore
        // dropped it out of Schedule or Cycle mode and wiped the
        // schedule behind it.
        //
        // That stayed invisible while the cache was never populated,
        // because the fields were empty on both sides. Now that the
        // app's own commands fill the cache, carrying the whole block
        // through is what keeps a speed change a speed change.
        //
        // The trade: the block is only as fresh as the last time the app
        // changed something, since the config poll is held back while the
        // cloud leg is up (see mitm_proxy.c). A stale modeType or
        // cycleTime is therefore written back on an unrelated speed
        // change. That is still the better failure: sending a partial
        // block loses those fields every single time, whereas this one
        // only matters if the controller was changed by some third route
        // the bridge never saw. The app and Home Assistant are both
        // observed, so in practice that leaves the controller's own
        // panel.
        cJSON *obj = fan_block_copy(mac, field);

        // fan_block_copy() has already normalised on/level onto
        // mOnOff/mLevel, so only the command spelling is read here.
        cJSON *v = cJSON_GetObjectItem(obj, "mOnOff");
        int cur_on = cJSON_IsNumber(v) ? (int)v->valuedouble : 1;
        v = cJSON_GetObjectItem(obj, "mLevel");
        int cur_level = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;

        int on, level;
        if (subfield && strcmp(subfield, "percentage") == 0) {
            bool ok = true;
            // The circulation fan is offered and published as a percent in
            // steps of ten and scaled here onto its wire 1-10; the blower
            // is already percent-like and taken as given. Auto on the
            // blower writes 0, its own "you choose" -- the same value the
            // app's Auto option means.
            // The running speed belongs to Manual mode. In Schedule, Cycle
            // and Environment the controller sets the speed itself (from
            // the schedule speed), so the value is read-only there, as in
            // the app. Unknown mode (nothing cached yet) counts as Manual.
            v = cJSON_GetObjectItem(obj, "modeType");
            if (cJSON_IsNumber(v) && (int)v->valuedouble != 0) {
                ESP_LOGW(TAG, "%s speed is read-only outside Manual mode", field);
                cJSON_Delete(obj);
                return NULL;
            }
            on = cur_on;
            if (atoi(value) == 0 && value[0] == '0') {
                // Home Assistant's fan slider sends 0 to mean "off".
                // Clamping it up to the lowest speed would leave the fan
                // running when it was asked to stop.
                on = 0;
                level = cur_level;
            } else {
                // Both in their own wire units (fan 1-10, blower 25-100).
                int v = clamp_int(value, speed_min, speed_max, &ok);
                if (!ok) { cJSON_Delete(obj); return NULL; }
                level = v;
                // Setting a speed on a stopped fan starts it, which is what
                // a slider move in Home Assistant means.
                on = 1;
            }
        } else {
            on = onoff_val(value);
            level = cur_level;
            // Same OFF -> ON restore as for lights: without it the fan
            // would come back on at speed 0, running but moving no air.
            if (on == 1 && level == 0) {
                level = device_cache_last_level(mac, field,
                                                strcmp(field, "blower") == 0 ? 50 : 5);
            }
        }

        // Only the manual speed. maxSpeed is the separate "Gang" used by
        // the scheduled modes and is left alone here: captured traffic
        // shows the two holding different values at the same time
        // ("maxSpeed":1 next to "mLevel":8), so writing both from one
        // slider would silently overwrite the schedule speed.
        set_num(obj, "mOnOff", on);
        set_num(obj, "mLevel", level);

        // Deliberately no force_schedule_enabled() here. The timePeriod
        // now comes from the controller's own last block rather than a
        // stub, so it already carries whatever "enabled" the app set —
        // and in Manual mode the app leaves that field out on purpose.
        return build_command(mac, uid, "device", field, obj);
    }

    // --- Climate accessories ---
    if (strcmp(field, "heater") == 0 || strcmp(field, "humidifier") == 0 ||
        strcmp(field, "dehumidifier") == 0) {

        device_cache_lock();
        cJSON *cached = device_cache_get(mac, field);
        int cur_level = 0;
        if (cached) {
            cJSON *v = cJSON_GetObjectItem(cached, "level");
            if (!v) v = cJSON_GetObjectItem(cached, "mLevel");
            cur_level = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
        }
        device_cache_unlock();

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "mOnOff", onoff_val(value));
        cJSON_AddNumberToObject(obj, "mLevel", cur_level);
        cJSON_AddItemToObject(obj, "timePeriod", default_time_period());
        return build_command(mac, uid, "device", field, obj);
    }

    ESP_LOGW(TAG, "Unknown command field '%s' subfield '%s'",
             field, subfield ? subfield : "-");
    return NULL;
}

// ===========================================================================
// Grow plan commands, as text
//
// The plan is never handled as one cJSON tree: five stages are ~5 KB of
// JSON but ~20 KB of nodes, which the heap cannot spare next to two TLS
// sessions. Stages are kept as separate strings (device_cache), at most one
// is parsed at a time, and the command is assembled with snprintf.
// ===========================================================================

// Validates one stage text: an object with the fields a stage needs.
static bool stage_valid(const char *t, size_t l)
{
    if (l < 2 || t[0] != '{') return false;
    static const char *const OBJ[] = { "light1", "light2", "target" };
    const char *v;
    size_t vl;
    for (int k = 0; k < 3; k++)
        if (!jtext_find(t, l, OBJ[k], &v, &vl) || v[0] != '{') return false;
    if (jtext_int(t, l, "stageId", -1) < 0) return false;
    long a = jtext_int(t, l, "startDate", -1), b = jtext_int(t, l, "endDate", -1);
    if (a <= 0 || b < a) return false;
    // The controller knows colours 1-5 only.
    long c = jtext_int(t, l, "color", 1);
    if (c < 1 || c > 5) return false;
    return jtext_find(t, l, "label", &v, &vl) && v[0] == '"';
}

static void sort_stages(char **st, int n)
{
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0; j--) {
            long a = jtext_int(st[j - 1], strlen(st[j - 1]), "startDate", 0);
            long b = jtext_int(st[j], strlen(st[j]), "startDate", 0);
            if (b >= a) break;
            char *t = st[j - 1]; st[j - 1] = st[j]; st[j] = t;
        }
    }
}

// Builds the setConfigField command for these stages; NULL (with err) when
// it would not fit in one controller frame.
static char *plan_envelope(const char *mac, const char *uid, char **st, int n,
                           char *err, size_t errsz)
{
    size_t body = 0;
    for (int i = 0; i < n; i++) body += strlen(st[i]) + 1;
    char msg_id[32];
    make_msg_id(msg_id, sizeof(msg_id));
    // Head ~100 B + tail 24 B + msgId (<=31) + uid (<=63): 320 covers it.
    size_t cap = body + 320;
    if (cap > SB_PLAN_FRAME_MAX) {
        snprintf(err, errsz, "Plan too large for the controller link (%u of %u bytes)",
                 (unsigned)cap, (unsigned)SB_PLAN_FRAME_MAX);
        return NULL;
    }
    char *out = malloc(cap);
    if (!out) { snprintf(err, errsz, "Out of memory"); return NULL; }
    size_t o = (size_t)snprintf(out, cap,
        "{\"method\":\"setConfigField\",\"pid\":\"%s\",\"params\":{\"keyPath\":[\"plan\"],\"plan\":{\"stage\":[",
        mac);
    for (int i = 0; i < n; i++) {
        if (i) out[o++] = ',';
        size_t sl = strlen(st[i]);
        memcpy(out + o, st[i], sl);
        o += sl;
    }
    snprintf(out + o, cap - o, "]}},\"msgId\":\"%s\",\"uid\":\"%s\"}", msg_id, uid ? uid : "");
    return out;
}

static void free_stages(char **st, int n)
{
    for (int i = 0; i < n; i++) free(st[i]);
}

char *sf_plan_command_text(const char *field, const char *sub, const char *value,
                           const char *mac, const char *uid, char *err, size_t errsz)
{
    if (err && errsz) err[0] = '\0';
    char dummy[8];
    if (!err) { err = dummy; errsz = sizeof(dummy); }
    if (!value) value = "";

    // --- Start / stop ---
    if (strcmp(field, "plan") == 0 && sub && strcmp(sub, "enabled") == 0) {
        int on = (strcasecmp(value, "ON") == 0 || strcmp(value, "1") == 0) ? 1 : 0;
        char msg_id[32];
        make_msg_id(msg_id, sizeof(msg_id));
        char *out = malloc(224);
        if (!out) { snprintf(err, errsz, "Out of memory"); return NULL; }
        snprintf(out, 224,
            "{\"method\":\"setConfigField\",\"pid\":\"%s\",\"params\":{\"keyPath\":[\"plan\",\"enabled\"],"
            "\"enabled\":%d},\"msgId\":\"%s\",\"uid\":\"%s\"}", mac, on, msg_id, uid ? uid : "");
        device_cache_set_plan_enabled(mac, on);
        ESP_LOGI(TAG, "Grow plan %s for %s", on ? "started" : "stopped", mac);
        return out;
    }

    // The current stages, copied out of the cache.
    //
    // Every plan command rewrites the controller's whole stage list, built
    // from these. While the controller's plan has not been read yet (after
    // a boot or reconnect) the list is unknown -- treating that as "no
    // stages" would replace a real plan with just the new stage. Refused
    // until the plan is known; nothing is ever dropped implicitly.
    int n = device_cache_plan_count(mac);
    if (n < 0) {
        snprintf(err, errsz, "The controller's plan has not been read yet -- try again in a moment");
        return NULL;
    }
    char *st[SB_PLAN_MAX_STAGES + 1] = {0};
    for (int i = 0; i < n; i++) {
        st[i] = device_cache_plan_stage_dup(mac, i);
        if (!st[i]) { free_stages(st, i); snprintf(err, errsz, "Out of memory"); return NULL; }
    }

    if (strcmp(field, "plan_template") == 0 || (strcmp(field, "plan") == 0 && sub && strcmp(sub, "template") == 0)) {
        // value: "<template id>[:days]" -- also accepts the bare system key
        // ("veg") used by older Home Assistant buttons.
        // Forms: "veg", "sys:veg", "cust:2", each optionally "@<days>".
        char id[24] = "";
        int days = 0;
        strncpy(id, value, sizeof(id) - 1);
        char *at = strchr(id, '@');
        if (at) { days = atoi(at + 1); *at = '\0'; }
        if (!strchr(id, ':')) {
            char tmp[24];
            snprintf(tmp, sizeof(tmp), "sys:%.18s", id);
            strcpy(id, tmp);
        }
        if (n >= SB_PLAN_MAX_STAGES) {
            snprintf(err, errsz, "The plan already has %d stages (maximum)", SB_PLAN_MAX_STAGES);
            free_stages(st, n);
            return NULL;
        }
        int start = sf_plan_today_packed();
        if (!start) {
            snprintf(err, errsz, "The bridge's clock is not set yet -- try again shortly");
            free_stages(st, n);
            return NULL;
        }
        if (n) {
            long last_end = 0;
            for (int i = 0; i < n; i++) {
                long e = jtext_int(st[i], strlen(st[i]), "endDate", 0);
                if (e > last_end) last_end = e;
            }
            if (last_end) start = sf_plan_date_add_days((int)last_end, 1);
        }
        st[n] = plan_tpl_make_stage(id, start, days);
        if (!st[n]) {
            snprintf(err, errsz, "Unknown or empty template '%s'", id);
            free_stages(st, n);
            return NULL;
        }
        n++;
    } else if (strcmp(field, "plan") == 0 && sub && strcmp(sub, "stage") == 0) {
        // One stage, added or replacing the stage with the same stageId.
        size_t vl = strlen(value);
        if (!stage_valid(value, vl)) {
            snprintf(err, errsz, "Not a valid stage");
            free_stages(st, n);
            return NULL;
        }
        long id = jtext_int(value, vl, "stageId", -1);
        int at = -1;
        for (int i = 0; i < n; i++)
            if (jtext_int(st[i], strlen(st[i]), "stageId", -2) == id) at = i;
        if (at < 0 && n >= SB_PLAN_MAX_STAGES) {
            snprintf(err, errsz, "The plan already has %d stages (maximum)", SB_PLAN_MAX_STAGES);
            free_stages(st, n);
            return NULL;
        }
        char *copy = strdup(value);
        if (!copy) { free_stages(st, n); snprintf(err, errsz, "Out of memory"); return NULL; }
        if (at >= 0) { free(st[at]); st[at] = copy; }
        else st[n++] = copy;
    } else if (strcmp(field, "plan") == 0 && sub && strcmp(sub, "delete") == 0) {
        long id = atol(value);
        int at = -1;
        for (int i = 0; i < n; i++)
            if (jtext_int(st[i], strlen(st[i]), "stageId", -2) == id) at = i;
        if (at < 0) {
            snprintf(err, errsz, "No stage %ld in the plan", id);
            free_stages(st, n);
            return NULL;
        }
        free(st[at]);
        for (int i = at; i < n - 1; i++) st[i] = st[i + 1];
        st[--n] = NULL;
    } else if (strcmp(field, "plan") == 0 && sub && strcmp(sub, "clear") == 0) {
        free_stages(st, n);
        n = 0;
    } else {
        snprintf(err, errsz, "Unknown plan command");
        free_stages(st, n);
        return NULL;
    }

    sort_stages(st, n);
    for (int i = 1; i < n; i++) {
        long pe = jtext_int(st[i - 1], strlen(st[i - 1]), "endDate", 0);
        long ns = jtext_int(st[i], strlen(st[i]), "startDate", 0);
        if (ns <= pe) {
            snprintf(err, errsz, "Stages %d and %d overlap", i, i + 1);
            free_stages(st, n);
            return NULL;
        }
    }

    char *cmd = plan_envelope(mac, uid, st, n, err, errsz);
    if (!cmd) { free_stages(st, n); return NULL; }
    // Committed only now that it fits; the cache takes the strings.
    device_cache_plan_set_stages(mac, st, n);
    ESP_LOGI(TAG, "Grow plan for %s: %d stage(s), %u bytes", mac, n, (unsigned)strlen(cmd));
    return cmd;
}

// ===========================================================================
// Long plans: the bridge drives the controller's plan window
//
// With a long plan (plan_store) active for a controller, the controller is
// given only the stage running today and the next one. When today passes
// the end of the first, the window moves on. Checked every minute and on
// connect; the controller is written only when the window changes.
// ===========================================================================

// Writes stages [first, first+1] of the long plan to the controller.
bool sf_plan_window_push(const char *mac, const char *uid, int first)
{
    plan_store_info_t info;
    if (!plan_store_info(mac, &info) || first < 0 || first >= info.count) return false;
    char *st[2] = {0};
    int n = 0;
    for (int i = first; i < info.count && n < 2; i++) {
        char *buf = malloc(PLAN_STORE_STAGE_MAX + 8);
        if (!buf) break;
        if (plan_store_get(mac, i, buf, PLAN_STORE_STAGE_MAX + 8)) st[n++] = buf;
        else free(buf);
    }
    if (!n) return false;
    char err[96];
    char *cmd = plan_envelope(mac, uid, st, n, err, sizeof(err));
    bool ok = false;
    if (cmd) {
        ok = mitm_proxy_inject_command_to(mac, cmd, strlen(cmd));
        free(cmd);
    }
    if (ok) {
        // The cache mirrors what the controller now holds.
        device_cache_plan_set_stages(mac, st, n);
        plan_store_set_window(mac, first);
        if (info.running) {
            char *en = sf_plan_command_text("plan", "enabled", "ON", mac, uid, err, sizeof(err));
            if (en) { mitm_proxy_inject_command_to(mac, en, strlen(en)); free(en); }
        }
        ESP_LOGI(TAG, "Long plan for %s: stages %d-%d of %d on the controller",
                 mac, first + 1, first + n, info.count);
    } else {
        for (int i = 0; i < n; i++) free(st[i]);
        ESP_LOGW(TAG, "Long plan for %s: window not written (%s)", mac, cmd ? "not sent" : err);
    }
    return ok;
}

// Every minute: move each active long plan's window to today.
__attribute__((unused)) static void plan_window_hook(void)
{
#if !SB_LONG_PLAN
    return;   // the plan lives on the controller only (see sb_config.h)
#endif
    int today = sf_plan_today_packed();
    if (!today) return;
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        char mac[DEV_MAC_LEN] = "";
        bool on = false;
        device_registry_lock();
        device_entry_t *d = device_registry_at(i);
        if (d) { strncpy(mac, d->mac, sizeof(mac) - 1); on = d->online; }
        device_registry_unlock();
        if (!mac[0] || !on) continue;
        plan_store_info_t info;
        if (!plan_store_info(mac, &info) || !info.active) continue;
        int cur = plan_store_current(mac, today);
        if (cur < 0) {
            if (info.window >= 0 && info.running) {
                ESP_LOGI(TAG, "Long plan for %s has ended", mac);
                plan_store_set_running(mac, false);
            }
            continue;
        }
        if (cur != info.window) sf_plan_window_push(mac, mitm_proxy_uid_for(mac), cur);
    }
}

void sf_plan_window_start(void)
{
#if SB_LONG_PLAN
    config_poll_add_hook(plan_window_hook, 60);
#else
    // A long plan left active by an older firmware would otherwise rewrite
    // the controller's plan with a two-stage window on the next connect.
    // Cleared once at boot; the stored stages themselves stay untouched.
    int n = plan_store_deactivate_all();
    if (n) ESP_LOGW(TAG, "Bridge-driven plan switched off for %d controller(s); "
                         "the controller's own plan is used", n);
#endif
}

// On connect: put the right window on the controller straight away (the
// controller may have run the old window while the bridge was off).
void sf_plan_window_on_connect(const char *mac, const char *uid)
{
#if !SB_LONG_PLAN
    (void)mac; (void)uid;
    return;
#endif
    plan_store_info_t info;
    if (!plan_store_info(mac, &info) || !info.active) return;
    int today = sf_plan_today_packed();
    int cur = today ? plan_store_current(mac, today) : info.window;
    if (cur >= 0) sf_plan_window_push(mac, uid, cur);
}
