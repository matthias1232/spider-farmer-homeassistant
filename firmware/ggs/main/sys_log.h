#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// System log capture
//
// Mirrors ESP_LOGx output into a ring buffer so the normal firmware log
// is readable at http://<bridge-ip>/syslog, without a serial cable.
//
// The previous firmware also hooked esp_log_set_vprintf and locked up:
// the HTTP code that served the log itself logged, which re-entered the
// hook, and under memory pressure that path leaked sockets until no TCP
// connection could be accepted at all. Three rules keep that from
// happening again, enforced in sys_log.c:
//
//   1. A per-task recursion guard. A log call made while that task is
//      already inside the hook is dropped, not queued.
//   2. The hook never allocates and never takes a blocking lock. It
//      formats into a fixed stack buffer and copies into a preallocated
//      ring, under a short critical section.
//   3. The original vprintf is always called, so serial output is
//      unchanged even if capture is disabled or failing.
// ============================================================================

// Trimmed from 160. Most log lines are well under this; the few that
// are longer lose only their tail, which is usually a repeated prefix
// or a path. Saves ~1 KB across the ring at no practical cost.
#define SYSLOG_LINE_MAX   112

typedef struct {
    uint32_t seq;                   // monotonic; gaps mean entries were lost
    uint32_t ms;                    // milliseconds since boot
    char     text[SYSLOG_LINE_MAX];
} sys_log_entry_t;

// Allocates the ring and installs the hook. Call once, early.
void sys_log_init(void);

// Copies up to max_out entries newer than after_seq, oldest first.
// Returns how many were copied.
int sys_log_fetch(uint32_t after_seq, sys_log_entry_t *out, int max_out);

// Sequence number of the newest entry, 0 when empty.
uint32_t sys_log_latest_seq(void);

// Number of lines dropped because the ring wrapped before a reader
// caught up. A climbing number means the log view is missing lines.
uint32_t sys_log_dropped(void);

