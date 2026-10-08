#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sb_config.h"

// ============================================================================
// Live MQTT log
//
// Ring buffer of recent MQTT packets, shown at http://<bridge-ip>/log.
//
// Deliberately narrow in scope: it records MQTT traffic only, by explicit
// calls from the proxy. It does not hook into esp_log_set_vprintf(). The
// previous firmware did that, and the logging inside the viewer teardown
// path re-entered the same code, leaked sockets, and eventually stopped
// the device accepting any TCP connection at all.
// ============================================================================

typedef enum {
    LOG_DIR_UP = 0,     // controller -> bridge (mirrored on to the cloud)
    LOG_DIR_DOWN,       // cloud -> controller
    LOG_DIR_HA_OUT,     // raw packet forwarded to Home Assistant
    LOG_DIR_HA_IN,      // command received from Home Assistant
} log_dir_t;

typedef struct {
    uint32_t seq;                        // monotonic, lets clients spot gaps
    uint32_t ms;                         // milliseconds since boot
    uint8_t  dir;
    uint8_t  packet_type;                // MQTT control packet type
    // 64, measured rather than guessed: the controller topics are short
    // ("SF/GGS/CB/API/DOWN/A1B2C3D4E5F6", 31 chars) but the Home
    // Assistant side is not — "spiderfarmer/zelt_links/command/
    // calibration/temp/set" is 52 and grows with the device name.
    // 48 was tried first and would have silently truncated those,
    // making the log view quietly wrong; 80 was more than anything
    // needs. Discovery topics (~60) are logged elsewhere, not here.
    char     topic[64];
    char     payload[SB_LOG_ENTRY_PAYLOAD];
    uint16_t payload_len;                // real length before truncation
    bool     truncated;
} log_entry_t;

// Prepares the log. The buffer itself is only allocated while a viewer is
// polling (live_log_viewer_poll), and freed 30 s after the last poll.
void live_log_init(void);

// Called by the log page's data endpoint on every poll: keeps (or starts)
// recording.
void live_log_viewer_poll(void);

// True while a viewer is open and entries are being recorded.
bool live_log_active(void);

// Records one packet. Safe to call from the proxy and MQTT tasks.
// topic and payload may be NULL for packet types that carry neither.
void live_log_add(log_dir_t dir, uint8_t packet_type, const char *topic,
                  const uint8_t *payload, size_t payload_len);

// Convenience wrappers for the Home Assistant side, so the log shows the
// normalised topics next to the raw controller traffic they came from.
void live_log_add_ha_out(const char *topic, const uint8_t *payload, size_t len);
void live_log_add_ha_in(const char *topic, const uint8_t *payload, size_t len);

// Copies up to max_out entries newer than after_seq into out, oldest
// first. Returns how many were copied.
//
// Note the allocation this implies: max_out entries of log_entry_t, each
// holding a full payload. Prefer live_log_fetch_one() for anything that
// runs often — a batch of 32 is a 16 KB contiguous request, which fails
// on a fragmented heap while TLS sessions are up.
int live_log_fetch(uint32_t after_seq, log_entry_t *out, int max_out);

// Copies the oldest entry newer than after_seq. Returns false when there
// is none. Lets a caller stream the log without a large buffer.
bool live_log_fetch_one(uint32_t after_seq, log_entry_t *out);

// Sequence number of the newest entry, 0 when the log is empty.
uint32_t live_log_latest_seq(void);

// Human-readable MQTT packet type, e.g. "PUBLISH".
const char *live_log_packet_name(uint8_t packet_type);
