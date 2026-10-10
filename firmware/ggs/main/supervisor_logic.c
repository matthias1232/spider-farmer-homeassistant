// Decision logic of the supervisor, with no hardware dependency so that it is compiled and tested on a
// PC (firmware/ggs/host_test/supervisor_host_test.c). Used by supervisor.c.
#include "esp_system.h"
#include "supervisor.h"

bool sv_reason_is_unhealthy(int r)
{
    switch (r) {
    case ESP_RST_PANIC:     // crash, abort, stack overflow
    case ESP_RST_INT_WDT:   // interrupt watchdog
    case ESP_RST_TASK_WDT:  // task watchdog
    case ESP_RST_WDT:       // other watchdog (RTC, bootloader)
    case ESP_RST_BROWNOUT:  // power dipped
        return true;
    default:                // power on, software restart (also ours), external reset, deep sleep
        return false;
    }
}

bool sv_why_is_unhealthy(sv_why_t why)
{
    switch (why) {
    case SV_WHY_HEARTBEAT:     // a core task was stuck
    case SV_WHY_LOW_HEAP:      // a leak
    case SV_WHY_BLE_TIMEOUT:   // the Bluetooth boot stalled
        return true;
    default:                   // user, uplink outage, none
        return false;
    }
}

uint32_t sv_next_streak(uint32_t prev, bool prev_unhealthy, bool prev_was_healthy_run)
{
    // A run that stayed up long enough proves the firmware works, whatever ended it.
    if (prev_was_healthy_run) return prev_unhealthy ? 1 : 0;
    if (prev_unhealthy) return prev + 1;
    // A plain restart (user, update, supervisor) before the run was healthy: neither proof
    // nor a crash -- keep the count, so a restart loop cannot reset the evidence.
    return prev;
}

bool sv_task_stuck(uint32_t now, uint32_t last, uint32_t period_s)
{
    if (now < last) return false;
    return (now - last) > (period_s * 6u + 30u);
}
