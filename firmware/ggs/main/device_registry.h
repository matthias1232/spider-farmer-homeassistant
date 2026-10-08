#pragma once
#include <stdbool.h>
#include "sb_config.h"

// ============================================================================
// Known controllers
//
// Each controller is identified by the MAC address in its MQTT topic.
// The registry maps that MAC to two things:
//
//   slug   the identifier used in every MQTT topic and Home Assistant
//          entity id, e.g. "tent_left"
//   name   the label shown in Home Assistant and on the tabs,
//          e.g. "Left tent"
//
// Both default to the MAC, so a brand new controller works immediately,
// and both can be changed at any time from the web interface. Renaming
// republishes discovery under the new identifiers and removes the old
// ones, so Home Assistant follows along without leaving ghost entities.
//
// A deleted controller disappears from the interface and from Home
// Assistant. If it publishes again it is simply learned anew — deleting
// is a way to clean up, not a block list.
// ============================================================================

#define DEV_MAX            SB_MAX_DEVICES
#define DEV_MAC_LEN        16
#define DEV_SLUG_LEN       24
#define DEV_NAME_LEN       32

#define DEV_REASON_LEN     32

typedef struct {
    bool in_use;
    char mac[DEV_MAC_LEN];     // "A1B2C3D4E5F6", as it appears in the topic
    char slug[DEV_SLUG_LEN];   // topic and entity id component
    char name[DEV_NAME_LEN];   // human readable label
    bool discovery_sent;       // discovery published under the current slug
    bool online;               // a session is currently active

    // --- Connection history, since boot ---
    //
    // Deliberately not persisted. Writing to flash on every reconnect
    // would wear the device out when a link is flapping, which is
    // exactly when the numbers matter. Home Assistant's recorder keeps
    // the long-term history instead; these cover "what happened since
    // the bridge last started".
    uint32_t connected_since;  // seconds of bridge uptime when it connected
    uint32_t reconnects;       // completed sessions since boot
    uint32_t total_online_s;   // accumulated connected time
    char last_reason[DEV_REASON_LEN];  // why the last session ended

    // --- Sensor calibration ---
    //
    // The controller applies these to its own readings, so corrected
    // values reach the vendor app and cloud as well as Home Assistant.
    //
    // Held here because the controller never reports them back: a
    // getSysSta or getDevSta reply contains no calibration block, so the
    // only record of what was set is the one kept here.
    float cal_temp;   // °C
    float cal_humi;   // %
    float cal_co2;    // ppm, not a percentage
    float cal_ppfd;

    // --- Time zone ---
    //
    // Read from the controller's own getSysSta, which reports timezone,
    // TZ and tzoff. Written with setDevTimezone.
    char tz_name[32];          // "Europe/Berlin"
    char tz_posix[48];         // "CET-1CEST,M3.5.0,M10.5.0/3"
    bool tz_known;             // the controller has reported one

    // --- Account ---
    //
    // The Spider Farmer account uid the controller last reported itself
    // bound to. Kept so the controller can be bound again (setDevActive)
    // after it was unpaired: an unbound controller reports an empty uid.
    char uid[24];
} device_entry_t;

// Remembers a non-empty account uid for this device (persisted).
void device_registry_note_uid(const char *mac, const char *uid);
// The remembered uid, "" when none.
const char *device_registry_uid(const char *mac);

// Loads the stored devices. Call once at startup.
void device_registry_init(void);

// Writes a pending change of the device list to flash now (it is otherwise
// written ~0.3 s later by a background task). Call before a restart.
void device_registry_flush(void);

// Returns the entry for a MAC, creating it on first sight. NULL when the
// table is full.
device_entry_t *device_registry_resolve(const char *mac);

// Lookup without creating.
device_entry_t *device_registry_find(const char *mac);
device_entry_t *device_registry_find_by_slug(const char *slug);

// Read access for the web interface.
int device_registry_count(void);
device_entry_t *device_registry_at(int index);

// Renames a device. Passing an empty slug or name falls back to the MAC.
// Returns false when the slug collides with another device.
bool device_registry_rename(const char *mac, const char *slug, const char *name);

// Name and topic used when none is set: "GGS A1B2C3D4E5F6" and
// "ggs_a1b2c3d4e5f6".
void device_registry_default_name(const char *mac, char *out, size_t n);
void device_registry_default_slug(const char *mac, char *out, size_t n);

// Forgets a device. It reappears if that controller publishes again.
bool device_registry_remove(const char *mac);

// Marks whether a session is currently running for this device.
//
// reason explains an offline transition, e.g. "controller closed" or
// "silent too long"; it is ignored when going online and may be NULL.
// Going offline also increments the reconnect counter and adds the
// finished session to the accumulated online time.
void device_registry_set_online_why(const char *mac, bool online,
                                    const char *reason);

// Shorthand for the common case, without a reason.
void device_registry_set_online(const char *mac, bool online);

// Seconds this device has been connected, 0 when it is not.
uint32_t device_registry_connected_for(const char *mac);

// Remembers a calibration offset that was sent to a controller.
//
// Kept on this side because the controller never reports its
// calibration back — no getSysSta or getDevSta reply contains it — so
// without this the interface could set offsets but never show them.
void device_registry_set_cal(const char *mac, const char *field, float value);

// Reads the remembered offsets. Any may be NULL.
void device_registry_get_cal(const char *mac, float *temp, float *humi,
                             float *co2, float *ppfd);

// Records the zone a controller reported in getSysSta.
void device_registry_set_tz(const char *mac, const char *name,
                            const char *posix);

void device_registry_lock(void);
void device_registry_unlock(void);
