#pragma once
#include "cJSON.h"

// ============================================================================
// Home Assistant command -> controller setConfigField
//
// Port of the original bridge's command_handler.py.
//
// A command is never a single field. The controller expects a COMPLETE
// module block and silently discards anything partial — no error, no
// reply, nothing in the log. So every command is built by taking the
// cached block, changing one field, and sending the whole thing back.
//
// Topic layout: spiderfarmer/<id>/command/<field>[/<subfield>]/set
//
// Returns a new cJSON object the caller must delete, or NULL when the
// command could not be translated.
// ============================================================================
cJSON *sf_translate_command(const char *field, const char *subfield,
                            const char *value, const char *mac,
                            const char *uid);

// A note about something the last translated command adjusted on its own
// (e.g. schedule speed Auto -> 1 on leaving Environment mode); "" if none.
// Copied into out and cleared.
const char *sf_command_take_note(char *out, size_t n);

// ---------------------------------------------------------------------------
// Grow plan commands, built as text (never a cJSON tree of the whole plan).
//
//   field "plan", sub "enabled"  value ON/OFF       start / stop
//   field "plan", sub "stage"    value stage JSON   add or replace (by stageId)
//   field "plan", sub "delete"   value stageId      remove a stage
//   field "plan", sub "template" value <id>[@days]  append a stage from a template
//   field "plan_template"        value <id>[@days]  same (Home Assistant)
//   field "plan", sub "clear"                       remove all stages
//
// Template ids: sys:seedling|clone|veg|flower|dry, cust:0..4 (a bare
// "veg" means sys:veg). At most SB_PLAN_MAX_STAGES stages; checked before
// anything is cached. Returns the command text to inject (caller frees),
// or NULL with a message in err.
// ---------------------------------------------------------------------------
char *sf_plan_command_text(const char *field, const char *sub, const char *value,
                           const char *mac, const char *uid, char *err, size_t errsz);

// "Sync device time": setDevTimezone with the zone in force and the
// bridge's current UTC clock. False when not sent (clock not set, no
// session).
bool sf_sync_device_time(const char *mac, const char *uid);

// Long plans (plan_store): write the window starting at stage `first` to
// the controller; the minute hook that moves windows; the connect check.
bool sf_plan_window_push(const char *mac, const char *uid, int first);
void sf_plan_window_start(void);
void sf_plan_window_on_connect(const char *mac, const char *uid);

// Sends the bridge's own zone and daylight-saving choice to one
// controller. Does nothing unless "Apply this to every controller" is
// enabled in the settings.
//
// Called when a controller connects, and again when the setting changes,
// so a device that was offline at the time still catches up.
void sf_push_timezone(const char *mac, const char *uid);

// Same, for every connected controller. Used when the setting changes,
// so the change is visible at once rather than at the next reconnect.
void sf_push_timezone_all(void);

// Starts the periodic sync. While "apply to every controller" is on, it
// re-sends the bridge's zone every few seconds, so a controller changed
// from the vendor app drifts back within one interval.
void sf_timezone_sync_start(void);

// Compares what a controller reports against the bridge's own setting
// and corrects it if they differ.
//
// Called from the status path, where the controller's zone arrives with
// every system frame, so a change made elsewhere is undone within
// seconds rather than at the next sync interval. Does nothing unless
// "apply to every controller" is on, and nothing when they already
// match — so the common case costs one string comparison.
void sf_timezone_check(const char *mac, const char *their_name,
                       const char *their_posix);