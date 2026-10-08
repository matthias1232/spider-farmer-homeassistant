#include <string.h>
#include <stdio.h>
#include <math.h>

#include "esp_log.h"

#include "device_cache.h"
#include "ha_mqtt.h"
#include "sf_alarm.h"

static const char *TAG = "sf_alarm";

// Ranges and steps as the app offers them. Changing a line here changes the
// web page and the command clamp together -- tools/gen_discovery.py holds
// the matching table for Home Assistant.
//
// PPFD: the app shows a minimum field, but its command only ever carries
// vmax, so only the maximum is offered here; writing a vmin the app never
// sends would be guessing at what the controller does with it.
const alarm_range_t ALARM_RANGES[] = {
    //  key        label                    unit        min?   min lo/hi       max lo/hi        step
    { "temp",     "Air temperature",      "\u00b0C",  true,    0.0,   30.0,   18.0,   50.0,  1.0 },
    { "humi",     "Air humidity",         "%",        true,    0.0,   80.0,   50.0,  100.0,  1.0 },
    { "vpd",      "VPD",                  "kPa",      true,    0.0,    1.6,    0.5,    4.0,  0.1 },
    { "co2",      "CO\u2082",             "ppm",      true,  200.0, 1500.0,  450.0, 5000.0, 10.0 },
    { "ppfd",     "PPFD",                 "\u00b5mol/m\u00b2/s", false, 0.0, 3900.0, 100.0, 4000.0, 10.0 },
    { "tempSoil", "Substrate temperature","\u00b0C",  true,    0.0,   26.0,   13.0,   50.0,  1.0 },
    { "humiSoil", "Substrate moisture",   "%",        true,    0.0,   80.0,   25.0,  100.0,  1.0 },
    { "ECSoil",   "Substrate EC",         "mS/cm",    true,    0.0,    4.3,    1.2,   20.0,  0.1 },
};
const int ALARM_RANGE_COUNT = sizeof(ALARM_RANGES) / sizeof(ALARM_RANGES[0]);

const alarm_switch_t ALARM_SWITCHES[] = {
    { "devOffline",      "Sensor offline alarm" },
    { "lightTemp",       "Light over-temperature alarm" },
    { "dehumiWaterFull", "Dehumidifier water tank full" },
    { "humiWaterLess",   "Humidifier water low" },
    { "waterLeak",       "Water leak" },
    { "waterLess",       "Water shortage" },
};
const int ALARM_SWITCH_COUNT = sizeof(ALARM_SWITCHES) / sizeof(ALARM_SWITCHES[0]);

static void pub(const char *slug, const char *suffix, const char *value)
{
    char topic[160];
    snprintf(topic, sizeof(topic), "spiderfarmer/%s/state/alarm/%s", slug, suffix);
    ha_mqtt_publish_state(topic, value);
}

static void fmt_num(char *out, size_t sz, double v)
{
    if (fabs(v - round(v)) < 1e-6) snprintf(out, sz, "%d", (int)round(v));
    else snprintf(out, sz, "%.1f", v);
}

void sf_publish_alarm(const char *mac, const char *slug)
{
    if (!mac || !slug) return;

    // Copied out under the lock, published without it.
    device_cache_lock();
    cJSON *blk = device_cache_get(mac, "alarm");
    cJSON *copy = blk ? cJSON_Duplicate(blk, true) : NULL;
    device_cache_unlock();
    if (!copy) return;

    char sfx[48], val[24];
    for (int i = 0; i < ALARM_RANGE_COUNT; i++) {
        const alarm_range_t *r = &ALARM_RANGES[i];
        cJSON *g = cJSON_GetObjectItem(copy, r->key);
        if (!cJSON_IsObject(g)) continue;
        cJSON *en = cJSON_GetObjectItem(g, "enabled");
        snprintf(sfx, sizeof(sfx), "%s/enabled", r->key);
        pub(slug, sfx, (cJSON_IsNumber(en) && en->valuedouble != 0) ? "ON" : "OFF");
        cJSON *mn = cJSON_GetObjectItem(g, "vmin");
        if (r->has_min && cJSON_IsNumber(mn)) {
            snprintf(sfx, sizeof(sfx), "%s/min", r->key);
            fmt_num(val, sizeof(val), mn->valuedouble);
            pub(slug, sfx, val);
        }
        cJSON *mx = cJSON_GetObjectItem(g, "vmax");
        if (cJSON_IsNumber(mx)) {
            snprintf(sfx, sizeof(sfx), "%s/max", r->key);
            fmt_num(val, sizeof(val), mx->valuedouble);
            pub(slug, sfx, val);
        }
    }
    for (int i = 0; i < ALARM_SWITCH_COUNT; i++) {
        cJSON *v = cJSON_GetObjectItem(copy, ALARM_SWITCHES[i].key);
        if (!cJSON_IsNumber(v)) continue;
        pub(slug, ALARM_SWITCHES[i].key, v->valuedouble != 0 ? "ON" : "OFF");
    }
    cJSON_Delete(copy);
    ESP_LOGD(TAG, "Alarm state published for %s", slug);
}
