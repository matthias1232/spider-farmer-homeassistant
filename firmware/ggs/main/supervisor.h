#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Supervisor: the device must always come back on its own
//
// "Pull the plug" must never be the way to restart this bridge. The hardware
// watchdogs (task, interrupt, bootloader; see sdkconfig.defaults) catch a CPU
// that stops scheduling. This module catches what they cannot: everything is
// alive, but the bridge no longer does its job.
//
//   Heartbeats   core tasks call sv_beat() regularly. A task that has not
//                beaten for much longer than its period is stuck (a
//                deadlock on a mutex, a socket call that never returns).
//   Memory       free internal heap that stays far below the level a TLS
//                session needs means a leak: restart before it becomes a hang.
//   Boot loop    the number of restarts in a row that did not end in a
//                healthy run is counted in RTC memory. After SV_LOOP_LIMIT of
//                them the bridge starts in SAFE MODE: hotspot, web interface,
//                USB console only. Nothing that has crashed it can start, and
//                it can still be reached, configured and updated.
//   Reset reason kept across the restart, with a persistent counter, so the
//                cause of a restart can be read afterwards over USB and on the
//                status page instead of being guessed.
//
// A restart decided here goes through sv_restart(), which first tries a clean
// shutdown and, if that does not complete in a few seconds, resets the chip
// anyway: a hung restart is the one thing this module must not allow.
// ============================================================================

// Reasons a supervisor-initiated restart is recorded with.
typedef enum {
    SV_WHY_NONE = 0,
    SV_WHY_HEARTBEAT,     // a core task stopped beating
    SV_WHY_LOW_HEAP,      // heap stayed below the floor
    SV_WHY_BLE_TIMEOUT,   // the Bluetooth-only boot did not finish
    SV_WHY_USER,          // restart requested over USB or the web interface
    SV_WHY_UPLINK,        // uplink down for too long (wifi_apsta.c)
} sv_why_t;

// First thing in app_main(), before anything can fail. Reads the reset reason,
// updates the boot-loop counter and says whether to start in safe mode.
// Returns true = SAFE MODE (start only hotspot, web interface and USB console).
bool sv_boot(void);

// Called when the bridge has been up and healthy for SV_HEALTHY_S seconds:
// clears the boot-loop counter. Started by sv_start().
bool sv_safe_mode(void);

// Starts the supervisor task. Call at the end of startup.
void sv_start(void);

// A task registers once and then beats. period_s is how often it normally
// runs; it is declared stuck after 6 periods plus 30 s. Returns a slot id, or
// -1 when the table is full (the task then simply is not supervised).
int  sv_register(const char *name, uint32_t period_s);
void sv_beat(int slot);

// Restarts the chip. Records why, tries a clean stop, and forces the reset if
// that takes longer than SV_FORCE_MS. Never returns.
void sv_restart(sv_why_t why) __attribute__((noreturn));

// Diagnostics, for the USB console and the status page.
typedef struct {
    int      reset_reason;       // esp_reset_reason_t of this boot
    char     reset_text[24];     // "Power on", "Task watchdog", ...
    uint32_t boots;              // boots since the last factory reset (persistent)
    uint32_t crash_streak;       // restarts in a row without a healthy run
    uint32_t crashes;            // unhealthy resets (panic, watchdogs, brownout) in total
    bool     safe_mode;
    sv_why_t last_sv_why;        // why the supervisor itself restarted last time
    char     last_sv_task[16];   // which task, for SV_WHY_HEARTBEAT
    uint32_t uptime_s;
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t heap_largest;
} sv_info_t;
void sv_get_info(sv_info_t *out);

// ---- tunables (also used by the host test) --------------------------------
#define SV_LOOP_LIMIT        3        // unhealthy restarts in a row before safe mode
#define SV_HEALTHY_S         120      // uptime after which a boot counts as healthy
#define SV_FORCE_MS          4000     // clean-stop budget before the forced reset
#define SV_LOW_HEAP_FLOOR    (20 * 1024)   // below this ...
#define SV_LOW_HEAP_FOR_S    90       // ... for this long means a leak
#define SV_MAX_TASKS         8

// ---- pure decision logic, testable on a PC ---------------------------------
// Was the previous run unhealthy? (a reset reason that no healthy shutdown produces)
bool sv_reason_is_unhealthy(int esp_reset_reason);
// Was a supervisor-initiated restart the answer to something broken (stuck task, leak, stalled
// Bluetooth boot)? A restart the user asked for or an uplink outage is not.
bool sv_why_is_unhealthy(sv_why_t why);
// New streak value after a boot, given the previous streak and what ended the last run.
uint32_t sv_next_streak(uint32_t prev, bool prev_unhealthy, bool prev_was_healthy_run);
// Is this task stuck?
bool sv_task_stuck(uint32_t now_s, uint32_t last_beat_s, uint32_t period_s);
