#pragma once
#include <string.h>

// ============================================================================
// Time zone table
//
// Maps a zone name to its POSIX rules, e.g.
//   "Europe/Berlin" -> "CET-1CEST,M3.5.0,M10.5.0/3"
//
// Both halves are needed: the controller's setDevTimezone command takes
// the name as a label and the POSIX string as the rules that actually
// decide the offset and the daylight-saving switch.
//
// Generated from the vendor app's own sf_timezone.json so the values a
// controller receives match what the app would send.
// ============================================================================

typedef struct {
    const char *name;    // "Europe/Berlin"
    const char *posix;   // "CET-1CEST,M3.5.0,M10.5.0/3"
} tz_entry_t;

extern const tz_entry_t TZ_TABLE[];
extern const int TZ_TABLE_COUNT;

// POSIX rules for a zone name, or NULL when it is not in the table.
const char *tz_posix_for(const char *name);
