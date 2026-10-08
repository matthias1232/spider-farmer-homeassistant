#pragma once
#include <stdbool.h>
#include "cJSON.h"

// ============================================================================
// Controller JSON -> Home Assistant state topics
//
// Port of the original bridge's normalizer.py. Turns a getDevSta or
// getConfigField frame into the retained state topics Home Assistant
// subscribes to, e.g.
//
//   spiderfarmer/<id>/state/temperature        23.4
//   spiderfarmer/<id>/state/fan                {"state":"ON","percentage":7}
//   spiderfarmer/<id>/state/fan/mode_label     Manual
//   spiderfarmer/<id>/state/fan/schedule_start 06:00
//
// Topic names and payload shapes are byte-identical to the original, so
// existing Home Assistant entities, history and automations keep working.
// ============================================================================

// Publishes every state topic derivable from a status frame.
//
// mac identifies which controller the frame came from and is used to
// reach that device's caches; slug is the identifier that appears in the
// topics, which the user can rename at any time.
void sf_normalize_and_publish(const char *mac, const char *slug, cJSON *root);

// Publishes the per-field topics for one fan or blower block. Used both
// from the status path and right after a command, so the HA entities
// update immediately instead of waiting for the next status frame.
void sf_publish_fan_extras(const char *slug, const char *prefix, cJSON *block);

// Same for a light or light2 block.
void sf_publish_light_extras(const char *slug, const char *prefix, cJSON *block);

// Publishes the day cycle targets: the two times that separate day from
// night, and per sensor a day value, a night value and a deadband. These
// are the thresholds the environment modes steer towards, and the block is
// not part of any device module -- it is addressed as keyPath ["target"].
void sf_publish_target_extras(const char *slug, cJSON *target);

// Publishes the grow plan from the cache: enabled, the current stage (by
// date) with its label, colour, dates and the whole plan as JSON.
void sf_publish_plan(const char *mac, const char *slug);

// Packed plan date (year << 16 | month << 8 | day) -> "YYYY-MM-DD".
void sf_plan_date_str(int packed, char *out, size_t n);
// Today as a packed plan date, 0 when the clock is not set yet.
int sf_plan_today_packed(void);
// Packed plan date, days later (or earlier, with a negative count).
int sf_plan_date_add_days(int packed, int days);
// Days from packed date a to b (b - a).
int sf_plan_date_diff(int a, int b);

// Last non-zero oscillation level seen for a controller (default 5).
void sf_osc_remember(const char *slug, int lvl);
int  sf_osc_last(const char *slug);

// Publishes the sensor cleaning switch, its phase (Cleaning / Cooling down /
// Idle) and the remaining time of the current phase as H:MM:SS.
void sf_publish_sensor_cleaning(const char *mac, const char *slug);

// Publishes the main fan/blower JSON state topic.
void sf_publish_fan_state(const char *slug, const char *prefix, cJSON *block);

// Publishes the fan/blower speed as its own topic, as the percent the app
// shows it in. Separate from the state topic above because Auto is an
// option in the app's list but not a number, so the select needs the word
// where the fan entity needs a figure.
void sf_publish_fan_speed(const char *slug, const char *prefix, cJSON *block);

// Publishes the main light/light2 JSON state topic. The cached block is
// consulted for modeType, which some firmware versions omit from status
// frames — without it the Home Assistant effect dropdown would flip to
// the wrong mode on every update.
void sf_publish_light_state(const char *slug, const char *prefix,
                            cJSON *block, cJSON *cached);

// Helpers shared with the command handler.
const char *sf_on_off(cJSON *value);

// Times of day, wrapped at 24 h.
void sf_seconds_to_hhmm(int seconds, char *out, size_t out_sz);
int sf_hhmm_to_seconds(const char *hhmm);

// Durations, to the second and not wrapped. Cycle run and off times are
// set to the second in the vendor app, so minutes alone lose data.
void sf_seconds_to_hhmmss(int seconds, char *out, size_t out_sz);
int sf_hhmmss_to_seconds(const char *hhmmss);
