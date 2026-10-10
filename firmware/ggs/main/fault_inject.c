#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fault_inject.h"
#include "supervisor.h"

static const char *TAG = "fault";

typedef enum { F_PANIC, F_TASKWDT, F_INTWDT, F_DEADLOCK, F_LEAK, F_COUNT } fault_t;
static const char *const NAMES[F_COUNT] = { "panic", "taskwdt", "intwdt", "deadlock", "leak" };

static int index_of(const char *kind)
{
    for (int i = 0; i < F_COUNT; i++)
        if (kind && strcmp(kind, NAMES[i]) == 0) return i;
    return -1;
}

bool fault_known(const char *kind) { return index_of(kind) >= 0; }

static void fault_task(void *arg)
{
    fault_t f = (fault_t)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(400));
    ESP_LOGW(TAG, "FAULT INJECTION: %s (self-test over USB)", NAMES[f]);
    vTaskDelay(pdMS_TO_TICKS(100));   // let the log line leave the UART
    switch (f) {
    case F_PANIC:
        abort();
    case F_TASKWDT:
        for (;;) { }                  // never yields: the idle task of this core is starved
    case F_INTWDT:
        portDISABLE_INTERRUPTS();
        for (;;) { }                  // nothing can run, only the interrupt watchdog can end this
    case F_DEADLOCK:
        // A supervised task that simply stops reporting, like one stuck on a lock.
        sv_register("fault", 1);
        break;
    case F_LEAK: {
        // Keep ~14 KB free: below the supervisor's 20 KB floor, but enough for it to keep running.
        void *keep = NULL;
        while (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 14 * 1024) {
            void **blk = heap_caps_malloc(1024, MALLOC_CAP_INTERNAL);
            if (!blk) break;
            *blk = keep;              // linked list: stays reachable, like a real leak
            keep = blk;
        }
        ESP_LOGW(TAG, "heap now %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        break;
    }
    default:
        break;
    }
    vTaskDelete(NULL);
}

void fault_run(const char *kind)
{
    int i = index_of(kind);
    if (i < 0) return;
    // Pinned to core 0 and above the idle task, so taskwdt and intwdt hit the core under test.
    xTaskCreatePinnedToCore(fault_task, "fault", 3072, (void *)(intptr_t)i, 10, NULL, 0);
}
