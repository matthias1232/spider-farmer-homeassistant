#pragma once
#include <stdbool.h>
#include <stddef.h>

// ============================================================================
// Grow plan as text
//
// A plan with five stages is ~5 KB of JSON, ~20 KB as a cJSON tree -- more
// than the bridge can spare. So the plan is never parsed whole: these
// helpers find JSON values by scanning (aware of strings and nesting) and
// the cache keeps each stage as its own compact string. Only one stage is
// ever parsed at a time.
// ============================================================================

// Finds the value of "key" at the top level of the JSON object starting at
// obj (which must point at '{'). On success *val points at the value's
// first character and *vlen is its length. Nested objects/arrays/strings
// are skipped correctly.
bool jtext_find(const char *obj, size_t len, const char *key, const char **val, size_t *vlen);

// Length of the JSON value starting at p (object, array, string, number,
// literal), 0 when malformed or cut off within len.
size_t jtext_value_len(const char *p, size_t len);

// Integer value of "key" in a JSON object text; def when absent.
long jtext_int(const char *obj, size_t len, const char *key, long def);

// For a frame that contains a plan (a getConfigField reply with
// data.plan, or a setConfigField with params.plan): locates the plan
// object. Returns false when the frame has none.
bool jtext_plan_in_frame(const char *frame, size_t len, const char **plan, size_t *plen);
