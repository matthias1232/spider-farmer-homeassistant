#pragma once
#include <stdbool.h>
#include <stddef.h>

// Connects to the Home Assistant MQTT broker and subscribes to the
// command topics for the configured publish mode.
void ha_mqtt_start(void);

// Publishes one complete controller packet, unchanged, to
// spiderfarmer/<device_id>/raw. Only active in raw or both mode.
// Not retained: this is a live stream, not state.
void ha_mqtt_publish_raw(const uint8_t *payload, size_t len);

// Publishes a normalised state topic, retained. Only active in Home
// Assistant or both mode.
void ha_mqtt_publish_state(const char *topic, const char *value);

// Publishes the full Home Assistant discovery set, retained, so entities
// appear even if Home Assistant starts later.
void ha_mqtt_publish_discovery(void);

// Clears retained discovery topics by publishing empty payloads, which
// makes Home Assistant drop the entities instead of leaving them
// "unavailable". Used when switching away from Home Assistant mode.
void ha_mqtt_clear_discovery(void);

// Publishes the bridge's own availability topic.
void ha_mqtt_publish_availability(bool online);

// Stores the bridge's Home Assistant name and re-announces its entities so
// the existing device is relabelled.
void ha_mqtt_rename_bridge(const char *name);

// Publishes the availability of one controller, under its own slug.
void ha_mqtt_publish_device_availability(const char *slug, bool online);

// The last "new heap low below 25 KB" observation with its context, "" if
// none since boot. Shown on /status/data.
const char *ha_mqtt_heap_low_note(void);

// Re-publishes one device's calibration offsets (held in the device
// registry) to its MQTT state topics.
//
// The registry is RAM-only and resets to zero on every bridge restart,
// while the MQTT topics are retained and therefore can still show
// whatever was last set before that restart — correct right after a
// calibration change, permanently wrong after a reboot if nothing ever
// corrects it. Call this when a controller is (re)identified, so a
// stale retained value cannot outlive the state it actually reflects.
void ha_mqtt_publish_device_calibration(const char *mac, const char *slug);

// Publishes discovery for one device the first time it is seen, and
// marks it online. Safe to call repeatedly.
void ha_mqtt_announce_device(const char *mac);

// Moves a device to new identifiers: clears the retained topics of
// old_slug, then republishes everything under the current name. Only the
// Home Assistant side is affected — the controller and the vendor cloud
// keep using the MAC either way.
void ha_mqtt_rename_device(const char *mac, const char *old_slug);

// Removes a device's retained topics so Home Assistant drops it.
void ha_mqtt_forget_device(const char *slug);

// Whether the configured mode includes each layer.
bool ha_mqtt_ha_enabled(void);
bool ha_mqtt_raw_enabled(void);

// true once the broker connection is up.
bool ha_mqtt_is_connected(void);

// ---------------------------------------------------------------------------
// Broker status, for the web interface
//
// Separates the cases that look identical from outside but need opposite
// fixes: no broker configured, configured but unreachable, reachable but
// rejecting the credentials, or connected and publishing.
// ---------------------------------------------------------------------------
typedef struct {
    bool     configured;     // a broker address is set at all
    bool     connected;      // session with the broker is up right now
    char     broker[128];    // the configured URI
    char     last_error[96]; // empty while healthy
    uint32_t connects;       // successful connections since boot
    uint32_t disconnects;    // lost connections; climbing means instability
    uint32_t publishes;      // state messages sent
    uint32_t uptime_s;       // seconds on the current connection
    int      mode;           // publish mode in force
} ha_mqtt_status_t;

// One entry of the broker-connection history. 'what' is 'c' connect,
// 'd' disconnect and 'e' error; for errors, detail is the error_type and
// payload the socket errno or the server's connect return code. All this
// exists because a session that drops every few seconds leaves no other
// trace: counters round to zero and the syslog ring drowns in publish
// noise.
typedef struct {
    uint32_t seq;
    uint32_t ms;
    char     what;
    uint16_t detail;
    uint16_t payload;
    uint32_t publishes;
} ha_mqtt_hist_t;

// Fills up to max entries of the connection history, oldest first.
// Returns how many were written.
int ha_mqtt_history(ha_mqtt_hist_t *out, int max);

void ha_mqtt_get_status(ha_mqtt_status_t *out);

// Publishes the bridge's own settings (time zone, daylight saving, NTP
// server and current clock) so Home Assistant reflects what is in force.
// Called after any of them change.
void ha_mqtt_publish_bridge_state(void);

// Grow plan template chosen in Home Assistant, per controller (RAM only).
void ha_mqtt_set_plan_template_sel(const char *slug, const char *name_or_id);
void ha_mqtt_get_plan_template_sel(const char *slug, char *id_out, size_t n);
void ha_mqtt_publish_plan_template_sel(const char *slug);
// The template select's discovery config (options = template names), for
// one controller / all controllers. Used when templates change.
bool ha_mqtt_publish_plan_template_select(const char *slug);
void ha_mqtt_refresh_plan_template_selects(void);

// Re-sends one controller's discovery (e.g. after the template list changed
// so the select's options are current). Runs on its own short task.
void ha_mqtt_request_rediscovery(void);

// Publishes one controller's connection health: how long it has been
// connected, how often it reconnected, and why the last session ended.
void ha_mqtt_publish_connection(const char *mac, const char *slug);
