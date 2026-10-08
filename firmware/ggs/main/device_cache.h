#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "sb_config.h"

// ============================================================================
// Device state caches
//
// Commands to the controller are not single fields — they are complete
// module blocks. Sending only the field that changed makes the controller
// discard the whole command silently. So every command is built by taking
// the last known block, changing one field, and sending the result back.
//
// Everything is keyed by MAC, because several controllers can be
// connected at once and their blocks must never mix.
//
// Merge semantics throughout: getDevSta on some firmware versions carries
// only {on, level} for light and fan, and replacing the cached block
// would wipe modeType, schedules and PPFD settings that the next command
// depends on.
// ============================================================================

void device_cache_init(void);

// Merges one module block for one controller.
void device_cache_merge(const char *mac, const char *module, cJSON *block);

// Replaces one module block entirely. For blocks where an absent field is
// meaningful -- the alarm block marks a disabled alarm by leaving out its
// "enabled" -- a merge would keep the stale field from before.
void device_cache_replace(const char *mac, const char *module, cJSON *block);

// Feeds a whole "data" object: merges every known module in one call.
void device_cache_merge_data(const char *mac, cJSON *data);

// Returns a reference to a cached block, or NULL. The caller must hold
// the lock around any use of the returned pointer.
cJSON *device_cache_get(const char *mac, const char *module);

// Last non-zero level seen for a module, or the supplied fallback.
int device_cache_last_level(const char *mac, const char *module, int fallback);

// Sensor readings and outlet states from the most recent status frame.
// These are not module blocks, so they are kept separately.
void device_cache_set_live(const char *mac, cJSON *sensors, cJSON *outlets);
cJSON *device_cache_sensors(const char *mac);
cJSON *device_cache_outlets(const char *mac);

// Whether the temperature/humidity sensor cleaning cycle is running, plus
// whether that has ever been decided and when it last changed.
//
// The controller runs the two-hour heating and the five-minute cooldown
// itself and does not report either in the frames polled here, so the last
// decided value is kept. "known" separates an unknown state from a
// deliberate OFF, which a plain bool would conflate.
//
// Returns the value; *known and *changed_us are optional.
bool device_cache_sensor_cleaning(const char *mac, bool *known, int64_t *changed_us);
void device_cache_set_sensor_cleaning(const char *mac, bool on);

// The controller's own report: getDevSta.sensorHeating {phase, remainTime}.
// phase 0 idle, 1 cleaning (switch on), 2 cooldown (switch already off).
// Setting it also sets the switch state (on only in phase 1).
void device_cache_set_sensor_cleaning_phase(const char *mac, int phase, int remain_s);

// Returns false until the controller has reported the block once. The
// remaining time is counted down locally from the last report.
bool device_cache_sensor_cleaning_phase(const char *mac, int *phase, int *remain_s);

// --- Grow plan (keyPath ["plan"]) ---
//
// Kept as one compact JSON string per stage (never a tree of the whole
// plan). set_plan_text takes the plan object's text {enabled?, stage:[...]}:
// a stage array replaces the stored stages, an "enabled" updates the flag.
bool  device_cache_set_plan_text(const char *mac, const char *plan, size_t len);
void  device_cache_set_plan_enabled(const char *mac, int enabled);
int   device_cache_plan_enabled(const char *mac);   // -1 unknown
int   device_cache_plan_count(const char *mac);     // -1 unknown
bool  device_cache_plan_known(const char *mac);
// Copy of stage i's text (caller frees), NULL when out of range.
char *device_cache_plan_stage_dup(const char *mac, int i);
// Replaces all stages; takes ownership of the n strings.
bool  device_cache_plan_set_stages(const char *mac, char **stages, int n);
// The stored plan as text ('{"stage":[...]}', no "enabled") copied into
// out; returns the length, 0 when none or it does not fit. enabled gets
// the flag.
size_t device_cache_plan_text(const char *mac, char *out, size_t n, int *enabled);

void device_cache_lock(void);
void device_cache_unlock(void);

// Bumped whenever a cached value really changes (not on identical
// reports). The web page polls it cheaply and reloads only on a change.
// device_cache_bump() is for state kept elsewhere (registry, plan store).
#include <stdint.h>
uint32_t device_cache_version(void);
void device_cache_bump(void);

// Fan/blower mode in status frames. getDevSta carries modeType only when
// the mode is not Manual (firmware 3.20). device_cache_note_mode_command()
// marks that a command just changed the module's mode;
// device_cache_status_mode() is called for every status frame's fan/blower
// block and says what to do with its mode:
//   MODE_FRAME_IGNORE  within 15 s of a mode command: drop the frame's
//                      modeType (a frame built before the change would
//                      undo it)
//   MODE_FRAME_MANUAL  modeType absent and this controller is known to
//                      report it otherwise: read as Manual (0)
//   MODE_FRAME_USE     take the frame as it is
#include <stdbool.h>
enum { MODE_FRAME_USE = 0, MODE_FRAME_MANUAL = 1, MODE_FRAME_IGNORE = 2 };
void device_cache_note_mode_command(const char *mac, const char *module);
int device_cache_status_mode(const char *mac, const char *module, bool frame_has_mode);

// Drops everything for one controller, used when it is forgotten.
void device_cache_clear(const char *mac);
