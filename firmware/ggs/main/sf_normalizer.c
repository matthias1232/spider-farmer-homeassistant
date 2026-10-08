#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"

#include "sb_config.h"
#include "ha_mqtt.h"
#include "device_cache.h"
#include "device_registry.h"
#include "mitm_proxy.h"
#include "sf_command_handler.h"
#include "time_sync.h"
#include "sf_normalizer.h"
#include "sf_alarm.h"
#include "plan_text.h"
#include "plan_store.h"

// Controller field name -> Home Assistant topic suffix.
static const struct { const char *sf; const char *ha; } SENSOR_MAP[] = {
    { "temp",     "temperature" },
    { "humi",     "humidity"    },
    { "vpd",      "vpd"         },
    { "co2",      "co2"         },
    { "ppfd",     "ppfd"        },
    { "tempSoil", "temp_soil"   },
    { "humiSoil", "humi_soil"   },
    { "ECSoil",   "ec_soil"     },
};
#define SENSOR_COUNT (sizeof(SENSOR_MAP) / sizeof(SENSOR_MAP[0]))

// modeType values for the fan block, confirmed against the Spider Farmer
// app. The five environment variants share the same modeType space as
// Manual/Schedule/Cycle rather than living in a separate field.
static const char *fan_mode_label(int mt)
{
    switch (mt) {
        case 0:  return "Manual";
        case 1:  return "Schedule";
        case 2:  return "Cycle";
        case 3:  return "Environment: Temperature only";
        case 4:  return "Environment: Humidity only";
        case 7:  return "Environment: Prioritize temperature";
        case 8:  return "Environment: Prioritize humidity";
        case 13: return "Environment: Temperature & humidity";
        default: return NULL;
    }
}

// Subset for the environment sub-mode dropdown.
//
// Returns NULL outside the environment modes. The caller then leaves the
// topic untouched rather than publishing an empty string: the sub-mode is
// a stored controller setting that survives a switch to Manual, and
// blanking it made the Home Assistant select drop to "unknown" every time
// the fan left Environment — the entity appeared to come and go. The app
// keeps showing the last choice, and so should this.
static const char *fan_env_label(int mt)
{
    switch (mt) {
        case 3:  return "Temperature only";
        case 4:  return "Humidity only";
        case 7:  return "Prioritize temperature";
        case 8:  return "Prioritize humidity";
        case 13: return "Temperature & humidity";
        default: return NULL;
    }
}

// The top level of the app's two-step mode picker: Manual, Schedule,
// Cycle, Environment. Every environment variant maps to "Environment",
// with the sub-mode select saying which one.
static const char *fan_main_mode_label(int mt)
{
    switch (mt) {
        case 0:  return "Manual";
        case 1:  return "Schedule";
        case 2:  return "Cycle";
        case 3:
        case 4:
        case 7:
        case 8:
        case 13: return "Environment";
        default: return "Manual";
    }
}

static const char *light_mode_label(int mt)
{
    switch (mt) {
        case 0:  return "Manual";
        case 1:  return "Schedule";
        case 12: return "PPFD";
        default: return NULL;
    }
}

const char *sf_on_off(cJSON *value)
{
    if (!value) return "OFF";
    if (cJSON_IsBool(value)) return cJSON_IsTrue(value) ? "ON" : "OFF";
    if (cJSON_IsNumber(value)) return value->valuedouble != 0 ? "ON" : "OFF";
    if (cJSON_IsString(value)) {
        const char *s = value->valuestring;
        if (strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0 ||
            strcasecmp(s, "true") == 0) return "ON";
    }
    return "OFF";
}

void sf_seconds_to_hhmm(int seconds, char *out, size_t out_sz)
{
    int s = seconds % 86400;
    if (s < 0) s += 86400;
    snprintf(out, out_sz, "%02d:%02d", s / 3600, (s % 3600) / 60);
}

int sf_hhmm_to_seconds(const char *hhmm)
{
    if (!hhmm) return 0;
    int h = 0, m = 0;
    if (sscanf(hhmm, "%d:%d", &h, &m) < 1) return 0;
    int s = (h * 3600 + m * 60) % 86400;
    return s < 0 ? s + 86400 : s;
}

// Durations, as opposed to times of day.
//
// Cycle run and off times are set to the second in the app — a captured
// run time of 03h 02min 02s is 10922 seconds. Rounding those to whole
// minutes loses the seconds, and because the command path now sends the
// whole cached block back, the rounded value would be written to the
// controller on the next unrelated change. Not wrapped at 24 h either:
// these are durations, and the app allows longer ones.
void sf_seconds_to_hhmmss(int seconds, char *out, size_t out_sz)
{
    if (seconds < 0) seconds = 0;
    snprintf(out, out_sz, "%02d:%02d:%02d",
             seconds / 3600, (seconds % 3600) / 60, seconds % 60);
}

int sf_hhmmss_to_seconds(const char *hhmmss)
{
    if (!hhmmss) return 0;
    int h = 0, m = 0, s = 0;
    int n = sscanf(hhmmss, "%d:%d:%d", &h, &m, &s);
    if (n < 1) return 0;
    if (n < 3) s = 0;
    if (n < 2) m = 0;
    int total = h * 3600 + m * 60 + s;
    return total < 0 ? 0 : total;
}

// Reads a numeric field, trying the status-frame name first and the
// config-frame name second. Firmware versions differ in which they send.
static double num_or(cJSON *obj, const char *key_a, const char *key_b, double def)
{
    cJSON *v = cJSON_GetObjectItem(obj, key_a);
    if (!cJSON_IsNumber(v) && key_b) v = cJSON_GetObjectItem(obj, key_b);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

// The slug is the per-device identifier the user can rename. Changing it
// moves every topic at once, which is exactly what renaming should do.
static void publish_fmt(const char *slug, const char *suffix,
                        const char *value)
{
    char topic[160];
    snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/%s", slug, suffix);
    ha_mqtt_publish_state(topic, value);
}

// ---------------------------------------------------------------------------
// Sensor cleaning (temperature/humidity sensor)
// ---------------------------------------------------------------------------
//
// Three topics, all derived from the same cached report:
//   state/sensor_cleaning         ON while cleaning (phase 1), else OFF
//   state/sensor_cleaning_status  Cleaning / Cooling down / Idle
//   state/sensor_cleaning_remain  remaining time of the current phase, H:MM:SS
// Published unchanged values are suppressed in ha_mqtt, so calling this
// every second only sends what actually changes.
void sf_publish_sensor_cleaning(const char *mac, const char *slug)
{
    int phase = 0, remain = 0;
    bool reported = device_cache_sensor_cleaning_phase(mac, &phase, &remain);
    bool known = false;
    bool on = device_cache_sensor_cleaning(mac, &known, NULL);
    if (!reported && !known) return;

    if (reported) on = (phase == 1);
    publish_fmt(slug, "sensor_cleaning", on ? "ON" : "OFF");

    const char *status = (phase == 1) ? "Cleaning"
                       : (phase == 2) ? "Cooling down" : "Idle";
    if (!reported) status = on ? "Cleaning" : "Idle";
    publish_fmt(slug, "sensor_cleaning_status", status);

    char t[40];
    if (phase == 0 || !reported) remain = 0;
    snprintf(t, sizeof(t), "%d:%02d:%02d", remain / 3600, (remain / 60) % 60, remain % 60);
    publish_fmt(slug, "sensor_cleaning_remain", t);

    // End of the current phase as an ISO timestamp (UTC), for the smooth
    // relative countdown in Home Assistant. Rounded to whole seconds and
    // only republished when it moves by more than 2 s, so the per-second
    // refresh does not resend it every time the local estimate jitters.
    static time_t last_end[SB_MAX_DEVICES];
    static char last_slug[SB_MAX_DEVICES][DEV_SLUG_LEN];
    int idx = -1;
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        if (strcmp(last_slug[i], slug) == 0) { idx = i; break; }
        if (idx < 0 && !last_slug[i][0]) idx = i;
    }
    if (idx >= 0 && !last_slug[idx][0]) {
        strncpy(last_slug[idx], slug, DEV_SLUG_LEN - 1);
        last_end[idx] = 0;
    }
    time_t now = time(NULL);
    if (remain > 0 && now > 1600000000) {
        time_t end = now + remain;
        if (idx < 0 || last_end[idx] == 0 ||
            llabs((long long)(end - last_end[idx])) > 2) {
            struct tm tm;
            gmtime_r(&end, &tm);
            strftime(t, sizeof(t), "%Y-%m-%dT%H:%M:%S+00:00", &tm);
            publish_fmt(slug, "sensor_cleaning_end", t);
            if (idx >= 0) last_end[idx] = end;
        }
    } else {
        publish_fmt(slug, "sensor_cleaning_end", "None");
        if (idx >= 0) last_end[idx] = 0;
    }
}

// ---------------------------------------------------------------------------
// Fan / blower extras
// ---------------------------------------------------------------------------

void sf_publish_fan_extras(const char *slug, const char *prefix, cJSON *block)
{
    if (!cJSON_IsObject(block)) return;

    char suffix[64];
    char val[32];

    cJSON *mt = cJSON_GetObjectItem(block, "modeType");
    if (cJSON_IsNumber(mt)) {
        int m = (int)mt->valuedouble;
        const char *label = fan_mode_label(m);
        snprintf(suffix, sizeof(suffix), "%s/mode_label", prefix);
        if (label) {
            publish_fmt(slug, suffix, label);
        } else {
            snprintf(val, sizeof(val), "%d", m);
            publish_fmt(slug, suffix, val);
        }

        // The four top-level modes the app's own dropdown offers. The
        // five environment variants all collapse to "Environment" here,
        // with the sub-mode select carrying which one — mirroring the
        // two-step layout of the app rather than the flat modeType space
        // the wire uses.
        snprintf(suffix, sizeof(suffix), "%s/main_mode", prefix);
        publish_fmt(slug, suffix, fan_main_mode_label(m));

        // Only while it is meaningful. Outside the environment modes the
        // topic keeps its previous value, so the select holds the last
        // choice instead of dropping to "unknown".
        const char *env = fan_env_label(m);
        if (env) {
            snprintf(suffix, sizeof(suffix), "%s/env_submode", prefix);
            publish_fmt(slug, suffix, env);
        }
    }

// Standby, as the app presents it: a number in its own range, or the
    // word Off where the controller holds 0.
    //
    // The blower's 0 means off; the circulation fan's standby is fixed at
    // 0 by definition, so both read as Off rather than as a scaleable
    // value.
    cJSON *v = cJSON_GetObjectItem(block, "minSpeed");
    snprintf(suffix, sizeof(suffix), "%s/standby_speed", prefix);
    if (strcmp(prefix, "fan") == 0) {
        // Fixed at 0 for the circulation fan, whether or not the controller
        // includes the field, so its select always has a state.
        publish_fmt(slug, suffix, "Off");
    } else if (cJSON_IsNumber(v)) {
        int sp = (int)v->valuedouble;
        if (sp <= 0) {
            publish_fmt(slug, suffix, "Off");
        } else {
            if (sp < 25) sp = 25;
            if (sp > 39) sp = 39;
            snprintf(val, sizeof(val), "%d", sp);
            publish_fmt(slug, suffix, val);
        }
    }

    // Schedule speed (maxSpeed): Auto (0) or the fan's step 1-10 / the
    // exhaust's 25-100 %. Clamped so the value is always one of the
    // select's options.
    v = cJSON_GetObjectItem(block, "maxSpeed");
    if (cJSON_IsNumber(v)) {
        snprintf(suffix, sizeof(suffix), "%s/schedule_speed", prefix);
        int w = (int)v->valuedouble;
        bool circ = (strcmp(prefix, "fan") == 0);
        if (w <= 0) {
            publish_fmt(slug, suffix, "Auto");
        } else {
            if (circ && w > 10) w = 10;
            if (!circ && w < 25) w = 25;
            if (!circ && w > 100) w = 100;
            snprintf(val, sizeof(val), "%d", w);
            publish_fmt(slug, suffix, val);
        }
    }

    // Oscillation as the app shows it: a switch, and a level 1-10. On the
    // wire it is one field, shakeLevel, where 0 means off. While it is
    // off the level keeps showing the last one used (remembered per
    // controller), so switching it on returns to that.
    v = cJSON_GetObjectItem(block, "shakeLevel");
    if (cJSON_IsNumber(v)) {
        int w = (int)v->valuedouble;
        if (w < 0) w = 0;
        if (w > 10) w = 10;
        snprintf(suffix, sizeof(suffix), "%s/oscillation", prefix);
        publish_fmt(slug, suffix, w > 0 ? "ON" : "OFF");
        if (w > 0) sf_osc_remember(slug, w);
        snprintf(suffix, sizeof(suffix), "%s/oscillation_level", prefix);
        snprintf(val, sizeof(val), "%d", w > 0 ? w : sf_osc_last(slug));
        publish_fmt(slug, suffix, val);
    }
    v = cJSON_GetObjectItem(block, "natural");
    if (v) {
        snprintf(suffix, sizeof(suffix), "%s/natural_wind", prefix);
        publish_fmt(slug, suffix, sf_on_off(v));
    }

    // Exhaust blower only. Whether the blower pauses while CO2 is being
    // added; the circulation fan's blocks never carry it, so the field
    // being absent is the normal case rather than missing data.
    v = cJSON_GetObjectItem(block, "closeCO2");
    if (v) {
        snprintf(suffix, sizeof(suffix), "%s/close_co2", prefix);
        publish_fmt(slug, suffix, sf_on_off(v));
    }

    // Schedule mode: timePeriod[0]
    //
    // Missing times are published as midnight rather than skipped. A
    // schedule window that has never been set reads as an empty box in
    // Home Assistant, which is indistinguishable from a broken form; 00:00
    // is at least the value the controller holds and can be typed over.
    cJSON *periods = cJSON_GetObjectItem(block, "timePeriod");
    bool have_schedule = false;
    if (cJSON_IsArray(periods) && cJSON_GetArraySize(periods) > 0) {
        cJSON *tp = cJSON_GetArrayItem(periods, 0);
        if (cJSON_IsObject(tp)) {
            have_schedule = true;
            cJSON *t = cJSON_GetObjectItem(tp, "startTime");
            if (cJSON_IsNumber(t)) {
                snprintf(suffix, sizeof(suffix), "%s/schedule_start", prefix);
                sf_seconds_to_hhmm((int)t->valuedouble, val, sizeof(val));
                publish_fmt(slug, suffix, val);
            }
            t = cJSON_GetObjectItem(tp, "endTime");
            if (cJSON_IsNumber(t)) {
                snprintf(suffix, sizeof(suffix), "%s/schedule_end", prefix);
                sf_seconds_to_hhmm((int)t->valuedouble, val, sizeof(val));
                publish_fmt(slug, suffix, val);
            }
        }
    }
    if (!have_schedule) {
        snprintf(suffix, sizeof(suffix), "%s/schedule_start", prefix);
        publish_fmt(slug, suffix, "00:00");
        snprintf(suffix, sizeof(suffix), "%s/schedule_end", prefix);
        publish_fmt(slug, suffix, "00:00");
    }

    // Cycle mode. Same reasoning: an unset cycle reports zero durations,
    // which is what the controller will use if the mode is selected.
    cJSON *ct = cJSON_GetObjectItem(block, "cycleTime");
    if (cJSON_IsObject(ct)) {
        cJSON *t = cJSON_GetObjectItem(ct, "startTime");
        if (cJSON_IsNumber(t)) {
            snprintf(suffix, sizeof(suffix), "%s/cycle_start", prefix);
            sf_seconds_to_hhmm((int)t->valuedouble, val, sizeof(val));
            publish_fmt(slug, suffix, val);
        }
        t = cJSON_GetObjectItem(ct, "openDur");
        if (cJSON_IsNumber(t)) {
            snprintf(suffix, sizeof(suffix), "%s/cycle_run_minutes", prefix);
            snprintf(val, sizeof(val), "%d", (int)t->valuedouble / 60);
            publish_fmt(slug, suffix, val);

            snprintf(suffix, sizeof(suffix), "%s/cycle_run_time", prefix);
            sf_seconds_to_hhmmss((int)t->valuedouble, val, sizeof(val));
            publish_fmt(slug, suffix, val);
        }
        t = cJSON_GetObjectItem(ct, "closeDur");
        if (cJSON_IsNumber(t)) {
            snprintf(suffix, sizeof(suffix), "%s/cycle_off_minutes", prefix);
            snprintf(val, sizeof(val), "%d", (int)t->valuedouble / 60);
            publish_fmt(slug, suffix, val);

            snprintf(suffix, sizeof(suffix), "%s/cycle_off_time", prefix);
            sf_seconds_to_hhmmss((int)t->valuedouble, val, sizeof(val));
            publish_fmt(slug, suffix, val);
        }
        t = cJSON_GetObjectItem(ct, "times");
        if (cJSON_IsNumber(t)) {
            snprintf(suffix, sizeof(suffix), "%s/cycle_times", prefix);
            snprintf(val, sizeof(val), "%d", (int)t->valuedouble);
            publish_fmt(slug, suffix, val);
        }
    } else {
        // An unset cycle still has to publish something, and 1 is what the
        // controller defaults to.
        snprintf(suffix, sizeof(suffix), "%s/cycle_start", prefix);
        publish_fmt(slug, suffix, "00:00");
        snprintf(suffix, sizeof(suffix), "%s/cycle_run_time", prefix);
        publish_fmt(slug, suffix, "00:00:00");
        snprintf(suffix, sizeof(suffix), "%s/cycle_off_time", prefix);
        publish_fmt(slug, suffix, "00:00:00");
        snprintf(suffix, sizeof(suffix), "%s/cycle_times", prefix);
        publish_fmt(slug, suffix, "1");
    }
}

// Last oscillation level per controller (by slug), for switching it back
// on at the level it had. RAM only; 5 is the default.
static struct { char slug[24]; uint8_t lvl; } s_osc[SB_MAX_DEVICES];

void sf_osc_remember(const char *slug, int lvl)
{
    int f = -1;
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        if (strcmp(s_osc[i].slug, slug) == 0) { s_osc[i].lvl = (uint8_t)lvl; return; }
        if (f < 0 && !s_osc[i].slug[0]) f = i;
    }
    if (f < 0) f = 0;
    strncpy(s_osc[f].slug, slug, sizeof(s_osc[f].slug) - 1);
    s_osc[f].lvl = (uint8_t)lvl;
}

int sf_osc_last(const char *slug)
{
    for (int i = 0; i < SB_MAX_DEVICES; i++)
        if (strcmp(s_osc[i].slug, slug) == 0 && s_osc[i].lvl) return s_osc[i].lvl;
    return 5;
}

void sf_publish_fan_state(const char *slug, const char *prefix, cJSON *block)
{
    if (!cJSON_IsObject(block)) return;

    cJSON *on = cJSON_GetObjectItem(block, "on");
    if (!on) on = cJSON_GetObjectItem(block, "mOnOff");
    int level = (int)num_or(block, "level", "mLevel", 0);

    // Speed in each module's own units, as the app shows them: the
    // circulation fan 1-10, the exhaust 25-100 %. The Home Assistant fan
    // entities declare those ranges (speed_range_min/max), so Home
    // Assistant maps them onto its slider itself.
    int pct = level;

    char payload[64];
    snprintf(payload, sizeof(payload), "{\"state\":\"%s\",\"percentage\":%d}",
             sf_on_off(on), pct);
    publish_fmt(slug, prefix, payload);
}

// The speed as its own topic, for the select that lists the app's own
// options. Auto has no percent, so it is published as the word rather than
// as a number, which is why this is separate from the fan entity's own
// percentage above.
void sf_publish_fan_speed(const char *slug, const char *prefix, cJSON *block)
{
    if (!cJSON_IsObject(block)) return;

    cJSON *lvl = cJSON_GetObjectItem(block, "level");
    if (!lvl) lvl = cJSON_GetObjectItem(block, "mLevel");
    if (!cJSON_IsNumber(lvl)) return;

    bool circulation = (strcmp(prefix, "fan") == 0);
    int v = (int)lvl->valuedouble;

    char suffix[64];
    char buf[16];
    snprintf(suffix, sizeof(suffix), "%s/speed", prefix);

    if (circulation) {
        // Scaled the same way the percentage above is, so the two agree.
        //
        // A stopped fan reports 0 on the wire, which scales to a 0% the
        // option list does not contain -- a select with no matching
        // option shows as unset. Held at the lowest speed instead, which
        // is where the fan returns to when it is switched back on.
        if (v > 10) v = 10;
        snprintf(buf, sizeof(buf), "%d", v < 1 ? 1 : v);
        publish_fmt(slug, suffix, buf);
        return;
    }

    // The running speed is always a number: the controller's own value,
    // 1:1. "Auto" belongs to the schedule speed (the scheduled and
    // environment modes), not to the speed the blower runs at now. A
    // stopped blower reports 0; the select keeps its lowest option then.
    if (v < 25) v = 25;
    if (v > 100) v = 100;
    snprintf(buf, sizeof(buf), "%d", v);
    publish_fmt(slug, suffix, buf);
}

// ---------------------------------------------------------------------------
// Day cycle targets -- the keyPath ["target"] block
//
// Sent as one flat object by the app, and the controller discards a partial
// block, so the publisher reads the whole thing and the command builder
// writes it back whole. dayTime holds the two times of day that separate the
// day target from the night target; temp, humi and co2 each carry a day
// value, a night value and a deadband.
// ---------------------------------------------------------------------------

void sf_publish_target_extras(const char *slug, cJSON *target)
{
    if (!cJSON_IsObject(target)) return;

    char suffix[80];
    char val[32];

    // Times of day, in seconds since midnight.
    cJSON *dt = cJSON_GetObjectItem(target, "dayTime");
    if (cJSON_IsObject(dt)) {
        cJSON *t = cJSON_GetObjectItem(dt, "startTime");
        if (cJSON_IsNumber(t)) {
            snprintf(suffix, sizeof(suffix), "target/day_time_start");
            sf_seconds_to_hhmm((int)t->valuedouble, val, sizeof(val));
            publish_fmt(slug, suffix, val);
        }
        t = cJSON_GetObjectItem(dt, "endTime");
        if (cJSON_IsNumber(t)) {
            snprintf(suffix, sizeof(suffix), "target/day_time_end");
            sf_seconds_to_hhmm((int)t->valuedouble, val, sizeof(val));
            publish_fmt(slug, suffix, val);
        }
    }

    // Each sensor: day target, night target, deadband.
    static const struct {
        const char *group;    // key in the target block
        const char *suffix;   // topic prefix
    } GROUPS[] = {
        { "temp", "target/temp" },
        { "humi", "target/humi" },
        { "co2",  "target/co2"  },
    };
    static const struct {
        const char *key;      // key inside the group
        const char *suffix;   // appended to the group prefix
    } FIELDS[] = {
        { "targetDay",   "_target_day"   },
        { "targetNight", "_target_night" },
        { "deadband",    "_deadband"     },
    };

    for (size_t g = 0; g < sizeof(GROUPS) / sizeof(GROUPS[0]); g++) {
        cJSON *grp = cJSON_GetObjectItem(target, GROUPS[g].group);
        if (!cJSON_IsObject(grp)) continue;
        for (size_t f = 0; f < sizeof(FIELDS) / sizeof(FIELDS[0]); f++) {
            cJSON *v = cJSON_GetObjectItem(grp, FIELDS[f].key);
            if (!cJSON_IsNumber(v)) continue;
            snprintf(suffix, sizeof(suffix), "%s%s",
                     GROUPS[g].suffix, FIELDS[f].suffix);
            // %g keeps 26 as "26" and 26.5 as "26.5", which is what a
            // number entity expects rather than a trailing ".000000".
            snprintf(val, sizeof(val), "%g", v->valuedouble);
            publish_fmt(slug, suffix, val);
        }
    }
}

// ---------------------------------------------------------------------------
// Grow plan
//
// {enabled, stage:[{stageId, label, startDate, endDate, alarmDate, color,
// light1, light2, target}]}. startDate/endDate are packed calendar days,
// (year << 16) | (month << 8) | day; alarmDate is epoch seconds. The
// current stage is the one whose day range contains today.
// ---------------------------------------------------------------------------

void sf_plan_date_str(int packed, char *out, size_t n)
{
    snprintf(out, n, "%04d-%02d-%02d", packed >> 16, (packed >> 8) & 255, packed & 255);
}

int sf_plan_today_packed(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2020) return 0;    // clock not set
    return ((t.tm_year + 1900) << 16) | ((t.tm_mon + 1) << 8) | t.tm_mday;
}
#define plan_today_packed sf_plan_today_packed

// Calendar arithmetic without mktime(): newlib's mktime did not normalise
// a day-of-month past the end of the month here (day 259 came back as
// "month 1, day 3" of the next year). Howard Hinnant's days-from-civil.
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, int *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

int sf_plan_date_add_days(int packed, int days)
{
    long z = days_from_civil(packed >> 16, (packed >> 8) & 255, packed & 255) + days;
    int y, m, d;
    civil_from_days(z, &y, &m, &d);
    return (y << 16) | (m << 8) | d;
}

int sf_plan_date_diff(int a, int b)
{
    return (int)(days_from_civil(b >> 16, (b >> 8) & 255, b & 255) -
                 days_from_civil(a >> 16, (a >> 8) & 255, a & 255));
}

// Publishes the plan's Home Assistant state from the cached stage texts.
// Only the one stage running today is parsed (one stage is ~4 KB as a
// tree; the whole plan would be up to ~20 KB).
void sf_publish_plan(const char *mac, const char *slug)
{
    int n = device_cache_plan_count(mac);
    if (n < 0) return;
    int on = device_cache_plan_enabled(mac);
    publish_fmt(slug, "plan/enabled", on > 0 ? "ON" : "OFF");
    char val[48];
    // With a long plan on the bridge, its stage count is the plan's.
    plan_store_info_t li;
    bool lp = SB_LONG_PLAN && plan_store_info(mac, &li) && li.active;
    snprintf(val, sizeof(val), "%d", lp ? li.count : n);
    publish_fmt(slug, "plan/stages", val);
    publish_fmt(slug, "plan/managed_by_bridge", lp ? "ON" : "OFF");

    // The stage running today (or the first one, when none matches),
    // found from the dates in the text.
    int today = plan_today_packed();
    int pick = n ? 0 : -1;
    for (int i = 0; i < n; i++) {
        char *t = device_cache_plan_stage_dup(mac, i);
        if (!t) continue;
        size_t tl = strlen(t);
        long a = jtext_int(t, tl, "startDate", -1), b = jtext_int(t, tl, "endDate", -1);
        free(t);
        if (a >= 0 && b >= 0 && today >= a && today <= b) { pick = i; break; }
    }
    char *txt = pick >= 0 ? device_cache_plan_stage_dup(mac, pick) : NULL;
    cJSON *cur = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    static const char *const COLORS[] = { "none", "green", "blue", "purple", "orange", "red" };
    if (cur) {
        cJSON *l = cJSON_GetObjectItem(cur, "label");
        publish_fmt(slug, "plan/stage_label", cJSON_IsString(l) ? l->valuestring : "");
        cJSON *a = cJSON_GetObjectItem(cur, "startDate");
        if (cJSON_IsNumber(a)) { sf_plan_date_str(a->valueint, val, sizeof(val)); publish_fmt(slug, "plan/stage_start", val); }
        cJSON *b = cJSON_GetObjectItem(cur, "endDate");
        if (cJSON_IsNumber(b)) { sf_plan_date_str(b->valueint, val, sizeof(val)); publish_fmt(slug, "plan/stage_end", val); }
        cJSON *c = cJSON_GetObjectItem(cur, "color");
        int ci = cJSON_IsNumber(c) ? c->valueint : 0;
        publish_fmt(slug, "plan/stage_color", (ci >= 0 && ci < 6) ? COLORS[ci] : "none");
        cJSON *al = cJSON_GetObjectItem(cur, "alarmDate");
        if (cJSON_IsNumber(al) && al->valuedouble > 0) {
            time_t ts = (time_t)al->valuedouble;
            struct tm t;
            localtime_r(&ts, &t);
            strftime(val, sizeof(val), "%Y-%m-%d %H:%M", &t);
            publish_fmt(slug, "plan/stage_reminder", val);
        } else {
            publish_fmt(slug, "plan/stage_reminder", "");
        }
        // Day in the stage, for a glance.
        if (cJSON_IsNumber(a) && today) {
            int days = sf_plan_date_diff(a->valueint, today) + 1;
            snprintf(val, sizeof(val), "%d", days);
            publish_fmt(slug, "plan/stage_day", val);
        }
        cJSON_Delete(cur);
    } else {
        publish_fmt(slug, "plan/stage_label", "");
        publish_fmt(slug, "plan/stage_day", "0");
    }
}

// ---------------------------------------------------------------------------
// Light extras
// ---------------------------------------------------------------------------

void sf_publish_light_extras(const char *slug, const char *prefix, cJSON *block)
{
    if (!cJSON_IsObject(block)) return;

    char suffix[64];
    char val[32];

    // Dim and off thresholds, as the select's own options: "Off" for the
    // controller's 0 ("never"), otherwise a whole degree in 15-50.
    //
    // Published even when absent, as Off: zero is a real setting -- a tent
    // that is never dimmed -- and a select with no state shows "unknown".
    // Publishing the bare number 0 did exactly that, since 0 is not one of
    // the options.
    const struct { const char *key; const char *topic; } TH[] = {
        { "darkTemp", "dim_threshold" },
        { "offTemp",  "off_threshold" },
    };
    for (size_t i = 0; i < sizeof(TH) / sizeof(TH[0]); i++) {
        cJSON *t = cJSON_GetObjectItem(block, TH[i].key);
        int deg = cJSON_IsNumber(t) ? (int)(t->valuedouble + 0.5) : 0;
        snprintf(suffix, sizeof(suffix), "%s/%s", prefix, TH[i].topic);
        if (deg <= 0) {
            publish_fmt(slug, suffix, "Off");
        } else {
            if (deg < 15) deg = 15;
            if (deg > 50) deg = 50;
            snprintf(val, sizeof(val), "%d", deg);
            publish_fmt(slug, suffix, val);
        }
    }

    cJSON *v;

    // Schedule: timePeriod[0]
    cJSON *periods = cJSON_GetObjectItem(block, "timePeriod");
    if (cJSON_IsArray(periods) && cJSON_GetArraySize(periods) > 0) {
        cJSON *tp = cJSON_GetArrayItem(periods, 0);
        if (cJSON_IsObject(tp)) {
            cJSON *t = cJSON_GetObjectItem(tp, "brightness");
            if (cJSON_IsNumber(t)) {
                snprintf(suffix, sizeof(suffix), "%s/schedule_brightness", prefix);
                snprintf(val, sizeof(val), "%d", (int)t->valuedouble);
                publish_fmt(slug, suffix, val);
            }
            t = cJSON_GetObjectItem(tp, "startTime");
            if (cJSON_IsNumber(t)) {
                snprintf(suffix, sizeof(suffix), "%s/schedule_start", prefix);
                sf_seconds_to_hhmm((int)t->valuedouble, val, sizeof(val));
                publish_fmt(slug, suffix, val);
            }
            t = cJSON_GetObjectItem(tp, "endTime");
            if (cJSON_IsNumber(t)) {
                snprintf(suffix, sizeof(suffix), "%s/schedule_end", prefix);
                sf_seconds_to_hhmm((int)t->valuedouble, val, sizeof(val));
                publish_fmt(slug, suffix, val);
            }
        }
    }

    // Sunrise/sunset ramp and the PPFD ramp, as the select's options: "Off"
    // for no ramp, otherwise whole minutes 1-60. Published even when the
    // period is missing, so the select never sits at "unknown".
    const struct { const char *arr; const char *topic; } FD[] = {
        { "timePeriod", "fade_minutes" },
        { "ppfdPeriod", "ppfd_fade_minutes" },
    };
    for (size_t i = 0; i < sizeof(FD) / sizeof(FD[0]); i++) {
        int mins = 0;
        cJSON *arr = cJSON_GetObjectItem(block, FD[i].arr);
        cJSON *p0 = (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0)
                        ? cJSON_GetArrayItem(arr, 0) : NULL;
        cJSON *ft = cJSON_IsObject(p0) ? cJSON_GetObjectItem(p0, "fadeTime") : NULL;
        if (cJSON_IsNumber(ft)) mins = ((int)ft->valuedouble + 30) / 60;
        snprintf(suffix, sizeof(suffix), "%s/%s", prefix, FD[i].topic);
        if (mins <= 0) {
            publish_fmt(slug, suffix, "Off");
        } else {
            if (mins > 60) mins = 60;
            snprintf(val, sizeof(val), "%d", mins);
            publish_fmt(slug, suffix, val);
        }
    }

    // PPFD mode: its own schedule plus brightness limits
    cJSON *pps = cJSON_GetObjectItem(block, "ppfdPeriod");
    if (cJSON_IsArray(pps) && cJSON_GetArraySize(pps) > 0) {
        cJSON *pp = cJSON_GetArrayItem(pps, 0);
        if (cJSON_IsObject(pp)) {
            cJSON *t = cJSON_GetObjectItem(pp, "brightness");
            if (cJSON_IsNumber(t)) {
                snprintf(suffix, sizeof(suffix), "%s/ppfd_target", prefix);
                snprintf(val, sizeof(val), "%d", (int)t->valuedouble);
                publish_fmt(slug, suffix, val);
            }
            t = cJSON_GetObjectItem(pp, "startTime");
            if (cJSON_IsNumber(t)) {
                snprintf(suffix, sizeof(suffix), "%s/ppfd_start", prefix);
                sf_seconds_to_hhmm((int)t->valuedouble, val, sizeof(val));
                publish_fmt(slug, suffix, val);
            }
            t = cJSON_GetObjectItem(pp, "endTime");
            if (cJSON_IsNumber(t)) {
                snprintf(suffix, sizeof(suffix), "%s/ppfd_end", prefix);
                sf_seconds_to_hhmm((int)t->valuedouble, val, sizeof(val));
                publish_fmt(slug, suffix, val);
            }
        }
    }

    v = cJSON_GetObjectItem(block, "ppfdMinBrightness");
    if (cJSON_IsNumber(v)) {
        snprintf(suffix, sizeof(suffix), "%s/ppfd_min", prefix);
        snprintf(val, sizeof(val), "%d", (int)v->valuedouble);
        publish_fmt(slug, suffix, val);
    }
    v = cJSON_GetObjectItem(block, "ppfdMaxBrightness");
    if (cJSON_IsNumber(v)) {
        snprintf(suffix, sizeof(suffix), "%s/ppfd_max", prefix);
        snprintf(val, sizeof(val), "%d", (int)v->valuedouble);
        publish_fmt(slug, suffix, val);
    }
}

void sf_publish_light_state(const char *slug, const char *prefix,
                            cJSON *block, cJSON *cached)
{
    if (!cJSON_IsObject(block)) return;

    cJSON *on = cJSON_GetObjectItem(block, "on");
    if (!on) on = cJSON_GetObjectItem(block, "mOnOff");
    int level = (int)num_or(block, "level", "mLevel", 0);

    // modeType comes from the frame when present, otherwise from the
    // cache. Defaulting to Manual would flip the Home Assistant effect
    // dropdown to the wrong mode every time a status frame without
    // modeType arrives — which some firmware versions always send.
    cJSON *mt = cJSON_GetObjectItem(block, "modeType");
    if (!cJSON_IsNumber(mt) && cJSON_IsObject(cached)) {
        mt = cJSON_GetObjectItem(cached, "modeType");
    }

    char payload[128];
    if (cJSON_IsNumber(mt)) {
        int m = (int)mt->valuedouble;
        const char *label = light_mode_label(m);
        char mode_buf[16];
        if (!label) {
            snprintf(mode_buf, sizeof(mode_buf), "%d", m);
            label = mode_buf;
        }
        snprintf(payload, sizeof(payload),
                 "{\"state\":\"%s\",\"brightness\":%d,\"effect\":\"%s\"}",
                 sf_on_off(on), level, label);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"state\":\"%s\",\"brightness\":%d}", sf_on_off(on), level);
    }
    publish_fmt(slug, prefix, payload);
}

// ---------------------------------------------------------------------------
// Full status frame
// ---------------------------------------------------------------------------

void sf_normalize_and_publish(const char *mac, const char *slug, cJSON *root)
{
    cJSON *d = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(d)) d = root;

    char suffix[64];
    char val[48];

    // --- Air sensors ---
    cJSON *sensor = cJSON_GetObjectItem(d, "sensor");
    if (cJSON_IsObject(sensor)) {
        for (size_t i = 0; i < SENSOR_COUNT; i++) {
            cJSON *v = cJSON_GetObjectItem(sensor, SENSOR_MAP[i].sf);
            if (cJSON_IsNumber(v)) {
                snprintf(val, sizeof(val), "%g", v->valuedouble);
                publish_fmt(slug, SENSOR_MAP[i].ha, val);
            }
        }
    }

    // --- Standalone light controller reports flat ---
    // It sends data.brightness and data.mode instead of nesting under
    // data.light, so map it onto the same topic rather than adding a
    // second code path in Home Assistant.
    cJSON *flat_brightness = cJSON_GetObjectItem(d, "brightness");
    if (cJSON_IsNumber(flat_brightness) && !cJSON_GetObjectItem(d, "light")) {
        int b = (int)flat_brightness->valuedouble;
        cJSON *mode = cJSON_GetObjectItem(d, "mode");
        char payload[128];
        if (cJSON_IsNumber(mode)) {
            int m = (int)mode->valuedouble;
            const char *label = light_mode_label(m);
            char mb[16];
            if (!label) { snprintf(mb, sizeof(mb), "%d", m); label = mb; }
            snprintf(payload, sizeof(payload),
                     "{\"state\":\"%s\",\"brightness\":%d,\"effect\":\"%s\"}",
                     b > 0 ? "ON" : "OFF", b, label);
        } else {
            snprintf(payload, sizeof(payload),
                     "{\"state\":\"%s\",\"brightness\":%d}",
                     b > 0 ? "ON" : "OFF", b);
        }
        publish_fmt(slug, "light", payload);
    }

    // --- Light blocks ---
    const char *light_names[] = { "light", "light2" };
    for (int i = 0; i < 2; i++) {
        cJSON *block = cJSON_GetObjectItem(d, light_names[i]);
        if (!cJSON_IsObject(block)) continue;

        // Copied under the lock, published without it: an MQTT publish can
        // block for seconds while the broker link is busy, and the web
        // server waits on this same lock -- the page then hung.
        device_cache_lock();
        cJSON *c0 = device_cache_get(mac, light_names[i]);
        cJSON *cached = c0 ? cJSON_Duplicate(c0, true) : NULL;
        device_cache_unlock();
        sf_publish_light_state(slug, light_names[i], block, cached);
        // Extras come from the cache, which has the schedule and PPFD
        // fields a plain status frame does not carry.
        sf_publish_light_extras(slug, light_names[i],
                                cached ? cached : block);
        cJSON_Delete(cached);
    }

    // --- Fan and blower ---
    const char *fan_names[] = { "blower", "fan" };
    for (int i = 0; i < 2; i++) {
        cJSON *block = cJSON_GetObjectItem(d, fan_names[i]);
        if (!cJSON_IsObject(block)) continue;

        sf_publish_fan_state(slug, fan_names[i], block);
            sf_publish_fan_speed(slug, fan_names[i], block);

        device_cache_lock();
        cJSON *c0 = device_cache_get(mac, fan_names[i]);
        cJSON *cached = c0 ? cJSON_Duplicate(c0, true) : NULL;
        device_cache_unlock();
        sf_publish_fan_extras(slug, fan_names[i], cached ? cached : block);
        cJSON_Delete(cached);
    }

    // --- Climate accessories ---
    const char *acc[] = { "heater", "humidifier", "dehumidifier" };
    for (int i = 0; i < 3; i++) {
        cJSON *block = cJSON_GetObjectItem(d, acc[i]);
        if (!cJSON_IsObject(block)) continue;
        cJSON *on = cJSON_GetObjectItem(block, "mOnOff");
        if (!on) on = cJSON_GetObjectItem(block, "on");
        if (on) publish_fmt(slug, acc[i], sf_on_off(on));
    }

    // --- Calibration, from a getConfigField reply ---
    //
    // The controller answers getConfigField with keyPath ["calibration"]
    // and returns the live offsets, including any the vendor app set.
    // That makes the displayed values the controller's own rather than a
    // guess based on what the bridge last sent.
    cJSON *cal = cJSON_GetObjectItem(d, "calibration");
    if (cJSON_IsObject(cal)) {
        device_cache_merge(mac, "calibration", cal);

        const struct { const char *key; const char *topic; } cf[] = {
            { "temp", "cal_temp" }, { "humi", "cal_humi" },
            { "co2",  "cal_co2"  }, { "ppfd", "cal_ppfd" },
        };
        for (int i = 0; i < 4; i++) {
            cJSON *v = cJSON_GetObjectItem(cal, cf[i].key);
            if (!cJSON_IsNumber(v)) continue;
            device_registry_set_cal(mac, cf[i].key, (float)v->valuedouble);

            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f", v->valuedouble);
            publish_fmt(slug, cf[i].topic, buf);
        }
    }

    // --- Sensor cleaning ---
    //
    // The controller owns the two-hour heating cycle and the five-minute
    // cooldown that follows it, and reports only whether the sensor is
    // currently being heated. It is read from the status frame when one
    // carries it; otherwise the value the last setSensorHeating command
    // set is republished, so the switch reflects what was asked for rather
    // than reverting to OFF between frames.
    //
    // getDevSta carries it as an object while a cycle runs or cools down:
    //   "sensorHeating":{"remainTime":7194,"phase":1}   cleaning (2 h)
    //   "sensorHeating":{"remainTime":288,"phase":2}    cooldown (5 min)
    // The switch is on only in phase 1: the cooldown counts as off, as in
    // the app. A frame without the block, or with phase 0, means idle --
    // but only once a frame with the block has been seen, so a controller
    // that never reports it keeps the state last commanded.
    cJSON *heat = cJSON_GetObjectItem(d, "sensorHeating");
    if (cJSON_IsObject(heat)) {
        cJSON *ph = cJSON_GetObjectItem(heat, "phase");
        cJSON *rt = cJSON_GetObjectItem(heat, "remainTime");
        device_cache_set_sensor_cleaning_phase(mac,
            cJSON_IsNumber(ph) ? (int)ph->valuedouble : 0,
            cJSON_IsNumber(rt) ? (int)rt->valuedouble : 0);
    } else if (cJSON_IsNumber(heat) || cJSON_IsBool(heat)) {
        device_cache_set_sensor_cleaning(mac,
            cJSON_IsNumber(heat) ? heat->valuedouble != 0 : cJSON_IsTrue(heat));
    } else if (cJSON_IsObject(cJSON_GetObjectItem(d, "sensor"))) {
        // A full status frame without the block: nothing is running.
        int phase = 0;
        if (device_cache_sensor_cleaning_phase(mac, &phase, NULL) && phase != 0) {
            device_cache_set_sensor_cleaning_phase(mac, 0, 0);
        }
    }

    sf_publish_sensor_cleaning(mac, slug);

    // --- Day cycle targets, from a getConfigField reply ---
    //
    // Arrives as its own block, addressed keyPath ["target"], rather than
    // inside a device module. The controller answers with all twelve values
    // at once, so what reaches Home Assistant here is complete rather than
    // a partial set of fields.
    cJSON *target = cJSON_GetObjectItem(d, "target");
    if (cJSON_IsObject(target)) {
        device_cache_merge(mac, "target", target);
        sf_publish_target_extras(slug, target);
    }

    // --- Alarm settings, from a getConfigField ["alarm"] reply ---
    //
    // Replaced, not merged: a disabled alarm is marked by its "enabled"
    // being absent, which a merge would never clear. The getDevSta status
    // frame also has an "alarmLast" object, which is a different thing
    // (the last alarm that fired) and is not touched here.
    cJSON *alarm_cfg = cJSON_GetObjectItem(d, "alarm");
    if (cJSON_IsObject(alarm_cfg)) {
        device_cache_replace(mac, "alarm", alarm_cfg);
        sf_publish_alarm(mac, slug);
    }

    // (Grow plan frames are taken from their text in mitm_proxy and never
    // reach this function.)

    // --- Time zone, from getSysSta ---
    //
    // The controller reports its own zone with every system status
    // frame. Worth recording because schedule times are local: showing
    // them against the wrong zone is silently misleading.
    cJSON *sys = cJSON_GetObjectItem(d, "sys");
    if (cJSON_IsObject(sys)) {
        // Cached so the web interface can show these between frames.
        device_cache_merge(mac, "sys", sys);

        cJSON *tzn = cJSON_GetObjectItem(sys, "timezone");
        cJSON *tzp = cJSON_GetObjectItem(sys, "TZ");
        if (cJSON_IsString(tzn) || cJSON_IsString(tzp)) {
            device_registry_set_tz(mac,
                cJSON_IsString(tzn) ? tzn->valuestring : NULL,
                cJSON_IsString(tzp) ? tzp->valuestring : NULL);

            // Correct a mismatch the moment it is seen.
            //
            // The controller reports its zone with every system status
            // frame, so a change made from the vendor app shows up
            // within seconds. Acting here rather than waiting for the
            // periodic sync means the correction is immediate, and it
            // costs nothing when the values already agree.
            if (cJSON_IsString(tzp)) {
                sf_timezone_check(mac,
                    cJSON_IsString(tzn) ? tzn->valuestring : "",
                    tzp->valuestring);
            }
        }
        if (cJSON_IsString(tzn)) publish_fmt(slug, "timezone", tzn->valuestring);

        // Per-device summer-time switch state, for Home Assistant.
        //
        // A POSIX TZ string with switching rules (a comma followed by
        // M-dates) means the controller is following the zone's own
        // rules automatically, in which case "is summer time active
        // right now" is what the switch should show rather than a fixed
        // forced/not-forced flag. Without rules, the string's offset
        // directly says whether it is pinned to standard or summer.
        if (cJSON_IsString(tzp)) {
            const char *rules = tzp->valuestring;
            bool has_rules = strchr(rules, ',') != NULL;
            bool is_summer;
            if (has_rules) {
                time_status_t t;
                time_sync_get_status(&t);
                is_summer = t.is_dst;
            } else {
                // No rules: a name of 4+ letters before the offset is
                // taken as the summer abbreviation (CEST, BST, ...); a
                // bare 3-letter name (CET, EST, ...) is standard time.
                size_t letters = 0;
                while (rules[letters] &&
                       !(rules[letters] == '-' || rules[letters] == '+' ||
                         (rules[letters] >= '0' && rules[letters] <= '9'))) {
                    letters++;
                }
                is_summer = letters >= 4;
            }
            publish_fmt(slug, "dst_on", is_summer ? "2" : "1");
        }

        // --- Everything else the controller reports about itself ---
        //
        // Published as-is where the value is already readable, and
        // converted where it is not: a raw epoch or a seconds count
        // tells you nothing at a glance.
        // "timezone" is published above and deliberately not repeated
        // here: this loop would otherwise overwrite the zone name with
        // the TZ rules, since both come from the same sys object.
        const struct { const char *key; const char *topic; } sys_str[] = {
            { "localtime",  "localtime"    },  // "2026-10-04 19:16:46 ..."
            { "ver",        "fw_version"   },
            { "hwver",      "hw_version"   },
            { "buildTime",  "fw_built"     },
            { "TZ",         "tz_rules"     },
            { "verUpdateWho", "fw_updated_by" },
        };
        for (size_t i = 0; i < sizeof(sys_str) / sizeof(sys_str[0]); i++) {
            cJSON *v = cJSON_GetObjectItem(sys, sys_str[i].key);
            if (cJSON_IsString(v)) publish_fmt(slug, sys_str[i].topic,
                                               v->valuestring);
        }

        const struct { const char *key; const char *topic; } sys_num[] = {
            { "upCount",       "restarts"    },  // power cycles
            { "mem",           "free_memory" },
            { "tzoff",         "utc_offset"  },  // seconds east of UTC
            { "verUpdateNum",  "fw_updates"  },
            { "hwcode",        "hw_code"     },
        };
        for (size_t i = 0; i < sizeof(sys_num) / sizeof(sys_num[0]); i++) {
            cJSON *v = cJSON_GetObjectItem(sys, sys_num[i].key);
            if (!cJSON_IsNumber(v)) continue;
            char buf[24];
            snprintf(buf, sizeof(buf), "%lld", (long long)v->valuedouble);
            publish_fmt(slug, sys_num[i].topic, buf);
        }

        // Uptime, both raw and readable. A bare 85494 is accurate but
        // meaningless at a glance; "23h 44m" is what a person wants.
        cJSON *up = cJSON_GetObjectItem(sys, "upTime");
        if (cJSON_IsNumber(up)) {
            long long s = (long long)up->valuedouble;
            char buf[24];
            snprintf(buf, sizeof(buf), "%lld", s);
            publish_fmt(slug, "uptime", buf);

            char pretty[32];
            if (s >= 86400) {
                snprintf(pretty, sizeof(pretty), "%lldd %lldh",
                         s / 86400, (s % 86400) / 3600);
            } else if (s >= 3600) {
                snprintf(pretty, sizeof(pretty), "%lldh %lldm",
                         s / 3600, (s % 3600) / 60);
            } else {
                snprintf(pretty, sizeof(pretty), "%lldm", s / 60);
            }
            publish_fmt(slug, "uptime_text", pretty);
        }

        // The last firmware update, as a date rather than an epoch.
        cJSON *vut = cJSON_GetObjectItem(sys, "verUpdateTime");
        if (cJSON_IsNumber(vut) && vut->valuedouble > 0) {
            time_t t = (time_t)vut->valuedouble;
            struct tm lt2;
            localtime_r(&t, &lt2);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt2);
            publish_fmt(slug, "fw_updated", buf);
        }

        // Uplink signal, when the controller reports it.
        cJSON *wifi = cJSON_GetObjectItem(sys, "wifi");
        if (cJSON_IsObject(wifi)) {
            cJSON *rssi = cJSON_GetObjectItem(wifi, "rssi");
            if (cJSON_IsNumber(rssi)) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", (int)rssi->valuedouble);
                publish_fmt(slug, "wifi_rssi", buf);
            }
            cJSON *conn = cJSON_GetObjectItem(wifi, "isConnect");
            if (cJSON_IsNumber(conn)) {
                publish_fmt(slug, "wifi_connected",
                            conn->valuedouble ? "ON" : "OFF");
            }
        }
    }

    // --- Last alarm ---
    //
    // Published as raw numbers, deliberately.
    //
    // The controller reports its most recent alarm as
    //   {"id":6,"epoch":1789816717,"devType":16,"alarmType":3}
    // and nothing documents what those numbers mean. The Spider Farmer
    // app binary was searched for a code-to-text table and has none: it
    // carries the key names but no per-code labels, and its translation
    // table holds only generic strings ("Alarm Notifications",
    // "Low Water Warning") with nothing tying them to a number.
    //
    // A guessed label would be believed, so the codes are surfaced as
    // they are. The meaning can be established later by triggering a
    // known condition and reading the value it produces.
    cJSON *alarm = cJSON_GetObjectItem(d, "alarmLast");
    if (cJSON_IsObject(alarm)) {
        const struct { const char *sf; const char *ha; } af[] = {
            { "alarmType", "alarm_type"     },
            { "devType",   "alarm_dev_type" },
            { "id",        "alarm_id"       },
            { "epoch",     "alarm_epoch"    },
        };
        for (int i = 0; i < 4; i++) {
            cJSON *v = cJSON_GetObjectItem(alarm, af[i].sf);
            if (!cJSON_IsNumber(v)) continue;
            char buf[24];
            snprintf(buf, sizeof(buf), "%lld", (long long)v->valuedouble);
            publish_fmt(slug, af[i].ha, buf);
        }

        // A readable date alongside the raw epoch. The code stays, since
        // its meaning is still unproven, but the timestamp does not need
        // to be a mystery while that is worked out.
        cJSON *ep = cJSON_GetObjectItem(alarm, "epoch");
        if (cJSON_IsNumber(ep) && ep->valuedouble > 0) {
            time_t t = (time_t)ep->valuedouble;
            struct tm lt;
            localtime_r(&t, &lt);
            char when[32];
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &lt);
            publish_fmt(slug, "alarm_time", when);
        }
    }

    // --- Individual soil sensors ---
    cJSON *sensors = cJSON_GetObjectItem(d, "sensors");
    if (cJSON_IsArray(sensors)) {
        cJSON *s = NULL;
        cJSON_ArrayForEach(s, sensors) {
            cJSON *id = cJSON_GetObjectItem(s, "id");
            if (!cJSON_IsString(id) || strcmp(id->valuestring, "avg") == 0) continue;

            const struct { const char *sf; const char *ha; } soil[] = {
                { "tempSoil", "temp" }, { "humiSoil", "humi" }, { "ECSoil", "ec" },
            };
            for (int k = 0; k < 3; k++) {
                cJSON *v = cJSON_GetObjectItem(s, soil[k].sf);
                if (!cJSON_IsNumber(v)) continue;
                snprintf(suffix, sizeof(suffix), "soil_%s_%s",
                         id->valuestring, soil[k].ha);
                snprintf(val, sizeof(val), "%g", v->valuedouble);
                publish_fmt(slug, suffix, val);
            }
        }
    }

    // --- Outlets ---
    cJSON *outlet = cJSON_GetObjectItem(d, "outlet");
    if (cJSON_IsObject(outlet)) {
        cJSON *o = NULL;
        cJSON_ArrayForEach(o, outlet) {
            if (!o->string || o->string[0] != 'O') continue;
            const char *num = o->string + 1;
            if (!num[0]) continue;

            cJSON *on = cJSON_GetObjectItem(o, "mOnOff");
            if (!on) on = cJSON_GetObjectItem(o, "on");
            if (!on) continue;

            snprintf(suffix, sizeof(suffix), "outlet_%s", num);
            publish_fmt(slug, suffix, sf_on_off(on));
        }
    }
}
