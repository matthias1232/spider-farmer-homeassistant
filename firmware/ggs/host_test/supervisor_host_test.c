// Host test for the supervisor's decision logic (main/supervisor.c): the pure functions that decide
// whether a restart counts as a failed run, when the bridge goes into safe mode, and when a task
// counts as stuck. The hardware side (watchdogs, RTC memory) cannot be tested on a PC; the CI build
// and the on-board reboot test cover that.
#include <stdio.h>
#include <string.h>
#include "esp_stub.h"
#include "supervisor.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static uint32_t streak_after(const int *reasons, const sv_why_t *whys, const bool *healthy, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i++) {
        bool bad = sv_reason_is_unhealthy(reasons[i]) || sv_why_is_unhealthy(whys[i]);
        s = sv_next_streak(s, bad, healthy[i]);
    }
    return s;
}

int main(void)
{
    // reset reasons
    CHECK(sv_reason_is_unhealthy(ESP_RST_PANIC));
    CHECK(sv_reason_is_unhealthy(ESP_RST_INT_WDT));
    CHECK(sv_reason_is_unhealthy(ESP_RST_TASK_WDT));
    CHECK(sv_reason_is_unhealthy(ESP_RST_WDT));
    CHECK(sv_reason_is_unhealthy(ESP_RST_BROWNOUT));
    CHECK(!sv_reason_is_unhealthy(ESP_RST_POWERON));
    CHECK(!sv_reason_is_unhealthy(ESP_RST_SW));
    CHECK(!sv_reason_is_unhealthy(ESP_RST_EXT));

    // supervisor reasons
    CHECK(sv_why_is_unhealthy(SV_WHY_HEARTBEAT));
    CHECK(sv_why_is_unhealthy(SV_WHY_LOW_HEAP));
    CHECK(sv_why_is_unhealthy(SV_WHY_BLE_TIMEOUT));
    CHECK(!sv_why_is_unhealthy(SV_WHY_USER));
    CHECK(!sv_why_is_unhealthy(SV_WHY_UPLINK));
    CHECK(!sv_why_is_unhealthy(SV_WHY_NONE));

    // A) panic loop never healthy -> safe mode on the 3rd start
    { int r[5]; sv_why_t w[5]; bool h[5];
      for (int i = 0; i < 5; i++) { r[i] = ESP_RST_PANIC; w[i] = SV_WHY_NONE; h[i] = false; }
      CHECK(streak_after(r, w, h, 2) < SV_LOOP_LIMIT);
      CHECK(streak_after(r, w, h, 3) >= SV_LOOP_LIMIT);
      CHECK(streak_after(r, w, h, 5) == 5); }

    // B) a single crash after a long healthy run never leads to safe mode, however often it repeats
    { int r[8]; sv_why_t w[8]; bool h[8];
      for (int i = 0; i < 8; i++) { r[i] = ESP_RST_PANIC; w[i] = SV_WHY_NONE; h[i] = true; }
      CHECK(streak_after(r, w, h, 8) == 1); }

    // C) restarts the supervisor decides on in a loop end in safe mode, too
    { int r[6]; sv_why_t w[6]; bool h[6];
      for (int i = 0; i < 6; i++) { r[i] = ESP_RST_SW; w[i] = SV_WHY_HEARTBEAT; h[i] = false; }
      CHECK(streak_after(r, w, h, 3) >= SV_LOOP_LIMIT); }

    // D) the user restarting, or an uplink outage, never leads to safe mode
    { int r[10]; sv_why_t w[10]; bool h[10];
      for (int i = 0; i < 10; i++) { r[i] = ESP_RST_SW; w[i] = (i % 2) ? SV_WHY_USER : SV_WHY_UPLINK; h[i] = false; }
      CHECK(streak_after(r, w, h, 10) == 0); }

    // E) power cycling does not count
    { int r[4]; sv_why_t w[4]; bool h[4];
      for (int i = 0; i < 4; i++) { r[i] = ESP_RST_POWERON; w[i] = SV_WHY_NONE; h[i] = false; }
      CHECK(streak_after(r, w, h, 4) == 0); }

    // F) plain restarts in between do not erase the evidence of two crashes
    { int r[4] = { ESP_RST_PANIC, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_PANIC };
      sv_why_t w[4] = { SV_WHY_NONE, SV_WHY_USER, SV_WHY_NONE, SV_WHY_NONE };
      bool h[4] = { false, false, false, false };
      CHECK(streak_after(r, w, h, 4) >= SV_LOOP_LIMIT); }

    // G) an update whose new image crashes three times in a row ends in safe mode
    { int r[4] = { ESP_RST_SW, ESP_RST_PANIC, ESP_RST_PANIC, ESP_RST_PANIC };
      sv_why_t w[4] = { SV_WHY_USER, SV_WHY_NONE, SV_WHY_NONE, SV_WHY_NONE };
      bool h[4] = { true, false, false, false };
      CHECK(streak_after(r, w, h, 4) >= SV_LOOP_LIMIT); }

    // stuck-task rule: 6 periods + 30 s
    CHECK(!sv_task_stuck(100, 100, 1));
    CHECK(!sv_task_stuck(135, 100, 1));    // 35 s late, limit 36
    CHECK( sv_task_stuck(137, 100, 1));
    CHECK(!sv_task_stuck(100 + 6 * 600 + 30, 100, 600));
    CHECK( sv_task_stuck(100 + 6 * 600 + 31, 100, 600));
    CHECK(!sv_task_stuck(50, 100, 1));     // clock went backwards: never "stuck"

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("ALL supervisor host tests passed\n");
    return 0;
}