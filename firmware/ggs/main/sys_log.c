#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sb_config.h"
#include "provisioning.h"
#include "syslog_fwd.h"
#include "sys_log.h"

// How many lines to keep. Each entry is SYSLOG_LINE_MAX bytes, so this
// is a fixed cost paid once at startup rather than a growing allocation.
// Reduced from 80 to reclaim heap.
//
// At 80 entries this ring held about 13.4 KB (80 x ~168 bytes) — the
// single largest permanent allocation in the firmware, larger even than
// the MQTT log ring, and all of it purely for the system-log view.
//
// Free heap was measured bottoming out at 37 KB with both TLS sessions
// up, uncomfortably close to the 70 KB the proxy wants before accepting
// a connection and to the point where allocations start failing. Thirty
// entries is still a useful window of recent log lines and gives back
// roughly 8.4 KB.
#define SYSLOG_RING   30

static sys_log_entry_t *s_ring = NULL;
static int       s_head = 0;        // next slot to write
static int       s_count = 0;       // valid entries, up to SYSLOG_RING
static uint32_t  s_seq = 0;
static uint32_t  s_dropped = 0;
static bool      s_ready = false;

static vprintf_like_t s_prev_vprintf = NULL;


// Guards the ring. A spinlock, not a mutex: the hook runs from arbitrary
// tasks including ones that must not block, and the critical section is
// a few hundred bytes of memcpy.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Recursion guard, per task.
//
// Serving the log over HTTP produces log lines of its own. Without this,
// one such line re-enters the hook from inside the hook; the old
// firmware did exactly that and eventually stopped accepting TCP
// connections. Tracking the task handle rather than a plain flag means
// one task recursing does not silence logging from the others.
#define MAX_NESTED 8
static TaskHandle_t s_in_hook[MAX_NESTED] = {0};

static bool enter_hook(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool ok = false;

    portENTER_CRITICAL(&s_mux);
    bool already = false;
    for (int i = 0; i < MAX_NESTED; i++) {
        if (s_in_hook[i] == me) { already = true; break; }
    }
    if (!already) {
        for (int i = 0; i < MAX_NESTED; i++) {
            if (s_in_hook[i] == NULL) { s_in_hook[i] = me; ok = true; break; }
        }
    }
    portEXIT_CRITICAL(&s_mux);

    return ok;
}

static void leave_hook(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_NESTED; i++) {
        if (s_in_hook[i] == me) { s_in_hook[i] = NULL; break; }
    }
    portEXIT_CRITICAL(&s_mux);
}

// Strips ANSI colour codes, which the console uses but a web page would
// show as stray escape characters.
static void copy_clean(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_sz; i++) {
        if (src[i] == '\033') {
            while (src[i] && src[i] != 'm') i++;
            continue;
        }
        if (src[i] == '\r') continue;
        if (src[i] == '\n' && o == 0) continue;   // no leading blank lines
        dst[o++] = src[i];
    }
    // Trailing newline is implied by the entry itself.
    while (o > 0 && dst[o - 1] == '\n') o--;
    dst[o] = '\0';
}

static int syslog_vprintf(const char *fmt, va_list args)
{
    // Serial output first and unconditionally: capture must never cost
    // us the log we would have had anyway.
    int ret = 0;
    va_list copy;
    va_copy(copy, args);
    if (s_prev_vprintf) ret = s_prev_vprintf(fmt, args);

    if (!s_ready || !enter_hook()) {
        va_end(copy);
        return ret;
    }

    char line[SYSLOG_LINE_MAX + 48];
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n > 0) {
        char clean[SYSLOG_LINE_MAX];
        copy_clean(line, clean, sizeof(clean));

        if (clean[0]) {
            portENTER_CRITICAL(&s_mux);
            sys_log_entry_t *e = &s_ring[s_head];
            e->seq = ++s_seq;
            e->ms  = (uint32_t)(esp_timer_get_time() / 1000);
            memcpy(e->text, clean, sizeof(e->text));

            s_head = (s_head + 1) % SYSLOG_RING;
            if (s_count < SYSLOG_RING) {
                s_count++;
            } else {
                s_dropped++;   // overwrote an entry no reader had seen
            }
            portEXIT_CRITICAL(&s_mux);

            // Non-blocking hand-off to the forwarder; a no-op when no
            // syslog server is configured.
            syslog_fwd_push(clean);
        }
    }

    leave_hook();
    return ret;
}

void sys_log_init(void)
{
    if (s_ring) return;

    s_ring = calloc(SYSLOG_RING, sizeof(sys_log_entry_t));
    if (!s_ring) {
        ESP_LOGW("sys_log", "Not enough memory for the system log buffer");
        return;
    }

    s_prev_vprintf = esp_log_set_vprintf(syslog_vprintf);
    s_ready = true;

    // Remote forwarding is NOT started here. This runs before the network
    // stack exists, and any socket or DNS call at this point asserts in
    // lwIP ("tcpip_send_msg_wait_sem: Invalid mbox") and reboots the device
    // in a loop. app_main starts it once Wi-Fi is up.

    ESP_LOGI("sys_log", "System log capture active (%d lines)", SYSLOG_RING);
}

int sys_log_fetch(uint32_t after_seq, sys_log_entry_t *out, int max_out)
{
    if (!s_ready || !out || max_out <= 0) return 0;

    int n = 0;
    portENTER_CRITICAL(&s_mux);

    int start = (s_head - s_count + SYSLOG_RING) % SYSLOG_RING;
    for (int i = 0; i < s_count && n < max_out; i++) {
        sys_log_entry_t *e = &s_ring[(start + i) % SYSLOG_RING];
        if (e->seq > after_seq) out[n++] = *e;
    }

    portEXIT_CRITICAL(&s_mux);
    return n;
}

uint32_t sys_log_latest_seq(void)
{
    return s_seq;
}

uint32_t sys_log_dropped(void)
{
    return s_dropped;
}
