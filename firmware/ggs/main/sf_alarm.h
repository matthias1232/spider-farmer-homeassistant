#pragma once
#include <stdbool.h>
#include "cJSON.h"

// ============================================================================
// Alarm settings (keyPath ["alarm"])
//
// Captured from the app's setConfigField:
//   "alarm":{"temp":{"enabled":1,"vmin":16.0,"vmax":32.0},
//            "humi":{"vmin":40.0,"vmax":90.0}, ... "ppfd":{"vmax":4000.0},
//            "devOffline":1,"dehumiWaterFull":1,"lightTemp":1,
//            "humiWaterLess":1,"waterLeak":1,"waterLess":1}
//
// Range alarms are objects with vmin/vmax; the alarm is on when "enabled"
// is 1 and off when "enabled" is absent (the app leaves it out). PPFD has
// only a maximum. The plain switches are 0/1 numbers at the top level.
//
// One table describes every field; the MQTT state, the commands, the web
// page and the Home Assistant entities are all derived from it.
// ============================================================================

typedef struct {
    const char *key;      // wire key: "temp", "humi", ...
    const char *label;    // shown to people
    const char *unit;
    bool        has_min;
    double      min_lo, min_hi;   // allowed range of vmin (as in the app)
    double      max_lo, max_hi;   // allowed range of vmax
    double      step;
} alarm_range_t;

typedef struct {
    const char *key;      // wire key: "devOffline", ...
    const char *label;
} alarm_switch_t;

extern const alarm_range_t  ALARM_RANGES[];
extern const int            ALARM_RANGE_COUNT;
extern const alarm_switch_t ALARM_SWITCHES[];
extern const int            ALARM_SWITCH_COUNT;

// Publishes every alarm state topic for one controller from the cached
// block:  state/alarm/<key>/enabled  ON|OFF
//         state/alarm/<key>/min      number
//         state/alarm/<key>/max      number
//         state/alarm/<switch>       ON|OFF
void sf_publish_alarm(const char *mac, const char *slug);
