#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "supervisor.h"

static const char *TAG = "supervisor";
#define NVS_NS "sbsv"

// ---------------------------------------------------------------------------
// State that survives a restart: RTC slow memory (kept over software, watchdog
// and panic resets; lost on power-off, where the streak is meaningless anyway).
// The magic tells garbage from a real record.
// ---------------------------------------------------------------------------
#define RTC_MAGIC 0x53560002u
typedef struct {
    uint32_t magic;
    uint32_t streak;             // unhealthy restarts in a row
    uint32_t healthy;            // the run that just ended reached SV_HEALTHY_S
    uint32_t sv_why;             // sv_why_t of a supervisor-initiated restart, 0 = none
    char     sv_task[16];
} sv_rtc_t;
static RTC_NOINIT_ATTR sv_rtc_t s_rtc;

static bool     s_safe = false;
static sv_info_t s_info;

typedef struct { char name[16]; uint32_t period_s; volatile uint32_t last_s; bool used; } slot_t;
static slot_t s_slots[SV_MAX_TASKS];

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

static void reset_text(int r, char *out, size_t n)
{
    const char *t = "Unknown";
    switch (r) {
    case ESP_RST_POWERON:   t = "Power on"; break;
    case ESP_RST_SW:        t = "Software restart"; break;
    case ESP_RST_PANIC:     t = "Crash"; break;
    case ESP_RST_INT_WDT:   t = "Interrupt watchdog"; break;
    case ESP_RST_TASK_WDT:  t = "Task watchdog"; break;
    case ESP_RST_WDT:       t = "Watchdog"; break;
    case ESP_RST_BROWNOUT:  t = "Power dipped"; break;
    case ESP_RST_DEEPSLEEP: t = "Deep sleep"; break;
    case ESP_RST_EXT:       t = "External reset"; break;
    }
    snprintf(out, n, "%s", t);
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
static void nvs_count(uint32_t *boots, uint32_t *crashes, bool bump_boot, bool bump_crash)
{
    nvs_handle_t h;
    uint32_t b = 0, c = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) { *boots = *crashes = 0; return; }
    nvs_get_u32(h, "boots", &b);
    nvs_get_u32(h, "crashes", &c);
    if (bump_boot)  { b++; nvs_set_u32(h, "boots", b); }
    if (bump_crash) { c++; nvs_set_u32(h, "crashes", c); }
    if (bump_boot || bump_crash) nvs_commit(h);
    nvs_close(h);
    *boots = b; *crashes = c;
}

bool sv_boot(void)
{
    int reason = (int)esp_reset_reason();
    if (s_rtc.magic != RTC_MAGIC || reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT) {
        // RTC memory is not trustworthy after a power-on; a brown-out can also scramble it.
        uint32_t keep = (s_rtc.magic == RTC_MAGIC && reason == ESP_RST_BROWNOUT) ? s_rtc.streak : 0;
        memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = RTC_MAGIC;
        s_rtc.streak = keep;
    }
    bool unhealthy = sv_reason_is_unhealthy(reason) || sv_why_is_unhealthy((sv_why_t)s_rtc.sv_why);
    s_rtc.streak = sv_next_streak(s_rtc.streak, unhealthy, s_rtc.healthy != 0);
    s_rtc.healthy = 0;

    memset(&s_info, 0, sizeof(s_info));
    s_info.reset_reason = reason;
    reset_text(reason, s_info.reset_text, sizeof(s_info.reset_text));
    s_info.crash_streak = s_rtc.streak;
    s_info.last_sv_why = (sv_why_t)s_rtc.sv_why;
    memcpy(s_info.last_sv_task, s_rtc.sv_task, sizeof(s_info.last_sv_task));
    s_info.last_sv_task[sizeof(s_info.last_sv_task) - 1] = '\0';
    s_rtc.sv_why = SV_WHY_NONE;
    s_rtc.sv_task[0] = '\0';

    nvs_count(&s_info.boots, &s_info.crashes, true, unhealthy);

    s_safe = s_rtc.streak >= SV_LOOP_LIMIT;
    s_info.safe_mode = s_safe;
    ESP_LOGW(TAG, "Start: %s, boot #%u, %u restart(s) in a row without a healthy run%s",
             s_info.reset_text, (unsigned)s_info.boots, (unsigned)s_rtc.streak,
             s_safe ? " -- SAFE MODE (hotspot, web interface and USB console only)" : "");
    if (s_info.last_sv_why != SV_WHY_NONE)
        ESP_LOGW(TAG, "The supervisor restarted the bridge last time: reason %d %s",
                 (int)s_info.last_sv_why, s_info.last_sv_task);
    return s_safe;
}

bool sv_safe_mode(void) { return s_safe; }

// ---------------------------------------------------------------------------
// Restart that cannot hang
// ---------------------------------------------------------------------------
static void force_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(SV_FORCE_MS));
    // The clean stop did not finish: reset the chip the hard way. esp_rom_software_reset_system()
    // does not take any lock, unlike esp_restart() which runs shutdown handlers first.
    esp_rom_printf("supervisor: clean stop timed out, forcing reset\n");
    esp_rom_software_reset_system();
    for (;;) {}
}

void sv_restart(sv_why_t why)
{
    s_rtc.sv_why = (uint32_t)why;
    // The owner stepping in (restart, update) is a fresh start: safe mode ends and the loop counter
    // begins again. If the image still crashes, three more starts bring safe mode back.
    if (sv_why_clears_streak(why)) s_rtc.streak = 0;
    ESP_LOGW(TAG, "Restarting (reason %d)", (int)why);
    // Armed BEFORE the clean path, so a clean path that blocks cannot prevent the reset.
    xTaskCreate(force_task, "sv_force", 2048, NULL, configMAX_PRIORITIES - 1, NULL);
    esp_restart();
    for (;;) {}
}

// ---------------------------------------------------------------------------
// Heartbeats and the supervisor task
// ---------------------------------------------------------------------------
int sv_register(const char *name, uint32_t period_s)
{
    for (int i = 0; i < SV_MAX_TASKS; i++) {
        if (!s_slots[i].used) {
            snprintf(s_slots[i].name, sizeof(s_slots[i].name), "%s", name ? name : "task");
            s_slots[i].period_s = period_s ? period_s : 1;
            s_slots[i].last_s = now_s();
            s_slots[i].used = true;
            return i;
        }
    }
    return -1;
}

void sv_beat(int slot)
{
    if (slot >= 0 && slot < SV_MAX_TASKS) s_slots[slot].last_s = now_s();
}

void sv_get_info(sv_info_t *out)
{
    if (!out) return;
    *out = s_info;
    out->uptime_s = now_s();
    out->heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->heap_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

static void supervisor_task(void *arg)
{
    uint32_t low_since = 0;
    bool healthy_marked = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t t = now_s();

        if (!healthy_marked && t >= SV_HEALTHY_S) {
            healthy_marked = true;
            s_rtc.healthy = 1;
            s_rtc.streak = 0;          // a run this long proves the firmware works
            s_info.crash_streak = 0;
            ESP_LOGI(TAG, "Healthy for %d s: restart counter cleared", SV_HEALTHY_S);
        }

        for (int i = 0; i < SV_MAX_TASKS; i++) {
            if (!s_slots[i].used) continue;
            if (sv_task_stuck(t, s_slots[i].last_s, s_slots[i].period_s)) {
                ESP_LOGE(TAG, "Task \"%s\" has not reported for %u s (expected every %u s) -- restarting",
                         s_slots[i].name, (unsigned)(t - s_slots[i].last_s), (unsigned)s_slots[i].period_s);
                snprintf(s_rtc.sv_task, sizeof(s_rtc.sv_task), "%s", s_slots[i].name);
                sv_restart(SV_WHY_HEARTBEAT);
            }
        }

        size_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (heap < SV_LOW_HEAP_FLOOR) {
            if (!low_since) low_since = t;
            if (t - low_since >= SV_LOW_HEAP_FOR_S) {
                ESP_LOGE(TAG, "Free heap below %u bytes for %u s (now %u) -- restarting",
                         (unsigned)SV_LOW_HEAP_FLOOR, (unsigned)(t - low_since), (unsigned)heap);
                sv_restart(SV_WHY_LOW_HEAP);
            }
        } else {
            low_since = 0;
        }
    }
}

void sv_start(void)
{
    // 3 KB is enough: the loop only compares numbers and calls two heap functions.
    xTaskCreate(supervisor_task, "supervisor", 3072, NULL, configMAX_PRIORITIES - 2, NULL);
}
