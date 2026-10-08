#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sb_config.h"
#include "mqtt_wire_parser.h"
#include "live_log.h"
#include "syslog_fwd.h"

static const char *TAG = "live_log";

// ---------------------------------------------------------------------------
// Storage: a byte arena plus a small index, instead of fixed-size slots.
//
// The previous ring held 8 fixed entries of ~530 bytes each. Every status
// frame makes the bridge publish 30-60 short Home Assistant state topics,
// so those 8 slots were overwritten several times between two polls of the
// log page, and a command sent in between was usually gone before the
// browser asked for it -- which is what "commands do not show up in the
// log" looked like.
//
// Here each record takes only the bytes it needs: a state update is ~70
// bytes, a full controller frame at most ~530. The same few kilobytes now
// hold around a hundred typical entries, and a long frame no longer costs
// as much as a short one.
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t seq;
    uint32_t ms;
    uint16_t off;          // start of topic+payload bytes in the arena
    uint16_t size;         // bytes used in the arena
    uint16_t payload_len;  // real length before truncation
    uint16_t stored_len;   // payload bytes actually kept
    uint8_t  dir;
    uint8_t  type;
    uint8_t  topic_len;
    uint8_t  truncated;
} rec_t;

// ---------------------------------------------------------------------------
// Only allocated while someone is watching.
//
// The buffer is just a hand-off to the browser, which keeps the history
// itself. With no log page open nothing is recorded and the memory is
// returned to the heap -- the TLS sessions need it more. The first poll
// from the log page allocates it; 30 s without a poll frees it again.
// Everything that reaches Home Assistant also stays readable on the broker,
// so nothing is lost by not recording while nobody looks.
// ---------------------------------------------------------------------------

static uint8_t *s_arena = NULL;
static rec_t   *s_recs = NULL;
static int      s_first = 0;     // index of the oldest record
static int      s_n = 0;         // records held
static uint16_t s_wpos = 0;      // next write offset in the arena
static uint32_t s_seq = 0;       // monotonic sequence number
static int64_t  s_last_poll_us = 0;
static SemaphoreHandle_t s_lock = NULL;

#define ARENA   SB_LOG_ARENA_BYTES
#define MAXREC  SB_LOG_MAX_RECORDS
#define IDLE_FREE_US (30LL * 1000 * 1000)

void live_log_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Live MQTT log on demand (%u bytes while a viewer is open)",
             (unsigned)(ARENA + MAXREC * sizeof(rec_t)));
}

// Caller holds the lock.
static void release_locked(void)
{
    free(s_arena); free(s_recs);
    s_arena = NULL; s_recs = NULL;
    s_first = s_n = 0;
    s_wpos = 0;
}

// Caller holds the lock. Returns false when memory is short; the viewer
// then simply gets nothing until the next try.
static bool ensure_locked(void)
{
    if (s_arena) return true;
    s_arena = malloc(ARENA);
    s_recs = calloc(MAXREC, sizeof(rec_t));
    if (!s_arena || !s_recs) {
        release_locked();
        return false;
    }
    ESP_LOGI(TAG, "Log viewer open: recording");
    return true;
}

// Frees the buffer once no viewer has polled for a while. Called on every
// record, so it needs no timer of its own.
static void maybe_release_locked(void)
{
    if (s_arena && esp_timer_get_time() - s_last_poll_us > IDLE_FREE_US) {
        release_locked();
        ESP_LOGI(TAG, "Log viewer closed: buffer released");
    }
}

void live_log_viewer_poll(void)
{
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    s_last_poll_us = esp_timer_get_time();
    ensure_locked();
    xSemaphoreGive(s_lock);
}

bool live_log_active(void)
{
    return s_arena != NULL;
}

static void evict_oldest(void)
{
    if (s_n == 0) return;
    s_first = (s_first + 1) % MAXREC;
    s_n--;
}

static bool overlaps_live(uint16_t start, uint16_t size)
{
    for (int i = 0; i < s_n; i++) {
        const rec_t *r = &s_recs[(s_first + i) % MAXREC];
        if (start < r->off + r->size && r->off < start + size) return true;
    }
    return false;
}

// Non-printable bytes become '.', so a binary payload cannot break the
// JSON sent to the browser.
static void copy_printable(uint8_t *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = src[i];
        dst[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
}

void live_log_add(log_dir_t dir, uint8_t packet_type, const char *topic,
                  const uint8_t *payload, size_t payload_len)
{
    // Copy to the syslog server first: that runs whether or not the log
    // page is open. A no-op unless the option is on.
    syslog_fwd_push_mqtt((int)dir, live_log_packet_name(packet_type), topic,
                         payload, payload ? payload_len : 0);

    // Unlocked fast path: with no viewer this is the whole cost of logging.
    if (!s_arena || !s_lock) return;

    size_t tl = topic ? strlen(topic) : 0;
    if (tl > sizeof(((log_entry_t *)0)->topic) - 1)
        tl = sizeof(((log_entry_t *)0)->topic) - 1;
    size_t pl = (payload && payload_len) ? payload_len : 0;
    if (pl > SB_LOG_ENTRY_PAYLOAD - 1) pl = SB_LOG_ENTRY_PAYLOAD - 1;

    uint16_t size = (uint16_t)(tl + pl);

    // Short timeout: logging must never hold up the relay loop.
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;

    maybe_release_locked();
    if (!s_arena) { xSemaphoreGive(s_lock); return; }

    // Records never wrap inside the arena: when the tail cannot hold this
    // one, it starts again at the front.
    if (s_wpos + size > ARENA) s_wpos = 0;
    while (s_n > 0 && (s_n >= MAXREC || overlaps_live(s_wpos, size))) {
        evict_oldest();
    }

    rec_t *r = &s_recs[(s_first + s_n) % MAXREC];
    r->seq = ++s_seq;
    r->ms = (uint32_t)(esp_timer_get_time() / 1000);
    r->off = s_wpos;
    r->size = size;
    r->payload_len = (uint16_t)(payload_len > 0xFFFF ? 0xFFFF : payload_len);
    r->stored_len = (uint16_t)pl;
    r->dir = (uint8_t)dir;
    r->type = packet_type;
    r->topic_len = (uint8_t)tl;
    r->truncated = payload_len > pl;

    if (tl) memcpy(s_arena + s_wpos, topic, tl);
    if (pl) copy_printable(s_arena + s_wpos + tl, payload, pl);

    s_wpos = (uint16_t)(s_wpos + size);
    s_n++;

    xSemaphoreGive(s_lock);
}

void live_log_add_ha_out(const char *topic, const uint8_t *payload, size_t len)
{
    live_log_add(LOG_DIR_HA_OUT, MQTT_PUBLISH, topic, payload, len);
}

void live_log_add_ha_in(const char *topic, const uint8_t *payload, size_t len)
{
    live_log_add(LOG_DIR_HA_IN, MQTT_PUBLISH, topic, payload, len);
}

static void to_entry(const rec_t *r, log_entry_t *out)
{
    memset(out, 0, sizeof(*out));
    out->seq = r->seq;
    out->ms = r->ms;
    out->dir = r->dir;
    out->packet_type = r->type;
    memcpy(out->topic, s_arena + r->off, r->topic_len);
    out->topic[r->topic_len] = '\0';
    memcpy(out->payload, s_arena + r->off + r->topic_len, r->stored_len);
    out->payload[r->stored_len] = '\0';
    out->payload_len = r->payload_len;
    out->truncated = r->truncated != 0;
}

int live_log_fetch(uint32_t after_seq, log_entry_t *out, int max_out)
{
    if (!s_arena || !s_lock || max_out <= 0) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    int n = 0;
    for (int i = 0; i < s_n && n < max_out; i++) {
        const rec_t *r = &s_recs[(s_first + i) % MAXREC];
        if (r->seq > after_seq) to_entry(r, &out[n++]);
    }

    xSemaphoreGive(s_lock);
    return n;
}

bool live_log_fetch_one(uint32_t after_seq, log_entry_t *out)
{
    // Single-entry fetch for the streaming HTTP handler: one entry at a
    // time keeps the request's memory to a single log_entry_t.
    if (!s_arena || !s_lock || !out) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return false;

    bool found = false;
    for (int i = 0; i < s_n; i++) {
        const rec_t *r = &s_recs[(s_first + i) % MAXREC];
        if (r->seq > after_seq) {
            to_entry(r, out);
            found = true;
            break;
        }
    }

    xSemaphoreGive(s_lock);
    return found;
}

uint32_t live_log_latest_seq(void)
{
    return s_seq;
}

const char *live_log_packet_name(uint8_t packet_type)
{
    switch (packet_type) {
        case MQTT_CONNECT:    return "CONNECT";
        case MQTT_CONNACK:    return "CONNACK";
        case MQTT_PUBLISH:    return "PUBLISH";
        case MQTT_PUBACK:     return "PUBACK";
        case MQTT_SUBSCRIBE:  return "SUBSCRIBE";
        case MQTT_SUBACK:     return "SUBACK";
        case MQTT_PINGREQ:    return "PINGREQ";
        case MQTT_PINGRESP:   return "PINGRESP";
        case MQTT_DISCONNECT: return "DISCONNECT";
        default:              return "-";
    }
}
