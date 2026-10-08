#pragma once
#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Hotspot access control
//
// Decides which Wi-Fi clients may use the bridge's hotspot. Three modes:
//
//   FILTER_OFF        every client may connect
//   FILTER_BLACKLIST  every client may connect except the listed ones
//   FILTER_WHITELIST  only the listed clients may connect
//
// The list holds MAC addresses and is stored on the device, so it survives
// a reboot. Enforcement happens in two places: the ESP32's own MAC filter
// in the Wi-Fi driver, and an explicit deauth when a blocked client still
// manages to associate.
//
// A deliberate safety rail: switching to whitelist mode with an empty list
// would lock out every client including the grow controller, so that
// combination is rejected.
// ============================================================================

#define MAC_FILTER_MAX_ENTRIES 16

typedef enum {
    FILTER_OFF = 0,
    FILTER_BLACKLIST,
    FILTER_WHITELIST,
} mac_filter_mode_t;

typedef struct {
    uint8_t mac[6];
    char    label[24];      // optional note, e.g. "grow controller"
    bool    in_use;
} mac_filter_entry_t;

// Information about a client currently associated with the hotspot.
typedef struct {
    uint8_t mac[6];
    int8_t  rssi;
    uint32_t ip;            // 0 when no DHCP lease has been handed out
    bool    listed;         // present in the filter list
} mac_client_t;

// Loads the stored mode and list. Call once after Wi-Fi is up.
void mac_filter_init(void);

mac_filter_mode_t mac_filter_get_mode(void);

// Applies a new mode and persists it. Returns false when the change was
// refused, which currently only happens for whitelist mode with an empty
// list.
bool mac_filter_set_mode(mac_filter_mode_t mode);

// Adds a MAC to the list. Accepts "AA:BB:CC:DD:EE:FF" and "AABBCCDDEEFF".
// Returns false on a malformed address or when the list is full.
bool mac_filter_add(const char *mac_str, const char *label);

// Removes a MAC from the list.
bool mac_filter_remove(const char *mac_str);

// Number of entries and read access by index.
int mac_filter_count(void);
const mac_filter_entry_t *mac_filter_entry_at(int index);

// Whether a MAC would be allowed under the current mode.
bool mac_filter_allows(const uint8_t mac[6]);

// Fills out with the clients currently associated with the hotspot.
// Returns how many were written.
int mac_filter_list_clients(mac_client_t *out, int max_out);

// Formats a MAC as "AA:BB:CC:DD:EE:FF". out needs 18 bytes.
void mac_filter_format(const uint8_t mac[6], char *out);

// Parses "AA:BB:CC:DD:EE:FF" or "AABBCCDDEEFF" into six bytes.
bool mac_filter_parse(const char *str, uint8_t out[6]);

// Disconnects any currently associated client that the active mode
// disallows. Called after the mode or the list changes.
void mac_filter_enforce(void);
