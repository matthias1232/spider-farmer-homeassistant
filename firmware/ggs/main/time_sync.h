#pragma once
#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Clock
//
// The bridge keeps its own time by NTP. This matters for two things:
//
//   1. TLS. Certificates have validity dates, and a device that believes
//      it is 1970 rejects every certificate it is offered.
//   2. Schedules. The controller expresses schedule and cycle times as
//      seconds since local midnight, so showing them correctly requires
//      knowing which local time is meant.
//
// Why the time zone is configured here rather than read from the
// controller: the controller never transmits it. Verified three ways —
// no clock fields appear anywhere in its live traffic, the original
// Python bridge has no such handling, and the protocol notes record that
// proxy-injected getConfigField requests time out unanswered. So the
// zone set here must match the one set in the Spider Farmer app, and the
// web interface says so plainly instead of implying it syncs.
//
// Daylight saving is handled by the POSIX TZ rules, e.g.
// "CET-1CEST,M3.5.0,M10.5.0/3" switches on the last Sunday in March and
// October without needing a network lookup.
// ============================================================================

typedef struct {
    bool     synced;          // NTP has set the clock at least once
    char     now[32];         // "2026-10-04 12:34:56"
    char     zone[48];        // the active TZ string
    char     abbrev[8];       // "CET" or "CEST"
    bool     is_dst;          // daylight saving in force right now
    int      utc_offset_min;  // minutes east of UTC, including any DST
    char     server[64];      // the NTP server in use
    uint32_t uptime_s;        // seconds since boot
} time_status_t;

// Starts NTP and applies the stored zone. Safe without a network: the
// zone takes effect immediately, the sync follows when a route exists.
void time_sync_start(void);

// Re-applies the zone after the settings change, without a restart.
void time_sync_apply_tz(void);

void time_sync_get_status(time_status_t *out);

// ---------------------------------------------------------------------------
// Settable over MQTT
//
// The same values the web interface exposes, writable from Home
// Assistant so the clock can be managed alongside everything else:
//
//   spiderfarmer/bridge/command/timezone/set   a POSIX TZ string
//   spiderfarmer/bridge/command/dst/set        Automatic | Standard | Summer
//   spiderfarmer/bridge/command/ntp_server/set a host name
//
// Each is applied immediately and persisted, and the resulting state is
// published back on spiderfarmer/bridge/state/... so Home Assistant
// always shows what is actually in force.
//
// Returns false when the value is rejected.
// ---------------------------------------------------------------------------
bool time_sync_set_timezone(const char *tz);
bool time_sync_set_dst_mode(const char *mode_label);
bool time_sync_set_ntp_server(const char *server);

// Label of the current mode: "Automatic", "Standard" or "Summer".
const char *time_sync_dst_label(void);
