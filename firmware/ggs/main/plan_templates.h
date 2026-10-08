#pragma once
#include <stdbool.h>
#include <stddef.h>

// ============================================================================
// Grow plan templates
//
// Five system templates (the app's own categories, read-only, in flash)
// and up to 40 custom templates stored in the "storage" partition, one
// 4 KB sector each. A template is a stage without dates:
//   {"name":"...","days":14,"color":1,"light1":{...},"light2":{...},"target":{...}}
// Nothing here is held in RAM.
//
// IDs: "sys:seedling", "sys:clone", "sys:veg", "sys:flower", "sys:dry",
//      "cust:0" .. "cust:39".
// ============================================================================

#define PLAN_TPL_SYSTEM 5
// One 4 KB flash sector each from 0x10000; 40 slots end at 0x38000. The
// rest of the partition (0x38000-0x60000, 160 KB) holds long grow plans.
#define PLAN_TPL_CUSTOM 40
#define PLAN_TPL_MAX_LEN 2048

// Template JSON text for id into out (NUL-terminated). Returns its length,
// 0 when the id is unknown or the custom slot is empty.
size_t plan_tpl_get(const char *id, char *out, size_t n);

// Display name of a template ("Seedling", a custom name), "" if none.
void plan_tpl_name(const char *id, char *out, size_t n);

// Stores a custom template (cust:N). json must be a template object; its
// "name" is replaced by name when given. Returns false on a bad id/json.
bool plan_tpl_save(const char *id, const char *name, const char *json);

// Empties a custom slot.
bool plan_tpl_delete(const char *id);

// Copies template src (system or custom) into the first free custom slot
// (or into dst when given) under name. Writes the new id to new_id.
bool plan_tpl_clone(const char *src, const char *dst, const char *name,
                    char *new_id, size_t n);

// Builds a stage text from a template: adds stageId, label (= the
// template's name), startDate, endDate (start + days - 1), alarmDate 0.
// days_override > 0 replaces the template's length. Returns a malloc'd
// string or NULL.
char *plan_tpl_make_stage(const char *id, int start_date, int days_override);

// Streams {"system":[...],"custom":[...|null]} to a callback in pieces.
typedef bool (*plan_tpl_sink_t)(void *ctx, const char *s, size_t n);
bool plan_tpl_list(plan_tpl_sink_t sink, void *ctx);

// The system ids and German labels, for selects (index 0..4).
const char *plan_tpl_system_id(int i);
