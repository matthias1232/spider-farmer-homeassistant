#pragma once
#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// DHCP leases and reservations
//
// Two related things:
//
//   * Leases    — which client currently holds which address. Read from
//                 the DHCP server, enriched with a name you assigned.
//   * Names     — a label per MAC, so the client list shows "grow
//                 controller" instead of a bare D0:CF:13:7A:60:B8.
//
// Reservations are handled by name plus a fixed address: the bridge's
// DHCP server hands out addresses from a pool, and a reserved client is
// kept at the same address across reboots by recording the pairing here
// and offering it back on the next request.
//
// The point of all this: once several devices are on the hotspot, a list
// of raw MAC addresses is close to useless for telling them apart.
// ============================================================================

#define DHCP_MAX_RESERVATIONS 12
#define DHCP_NAME_LEN         24

typedef struct {
    uint8_t  mac[6];
    uint32_t ip;                  // network byte order, 0 = no reservation
    char     name[DHCP_NAME_LEN]; // what this device is
    bool     in_use;
} dhcp_reservation_t;

// Loads the stored reservations. Call once after Wi-Fi is up.
void dhcp_leases_init(void);

// Name for a MAC, or NULL when none was assigned.
const char *dhcp_leases_name_for(const uint8_t mac[6]);

// Reserved address for a MAC, or 0 when the client may take any address.
uint32_t dhcp_leases_reserved_ip(const uint8_t mac[6]);

// Adds or updates an entry. Pass ip = 0 to only name a device without
// pinning its address. Returns false when the table is full or the MAC is
// malformed.
bool dhcp_leases_set(const char *mac_str, const char *name, const char *ip_str);

// Removes an entry.
bool dhcp_leases_remove(const char *mac_str);

// Read access for the web interface.
int dhcp_leases_count(void);
const dhcp_reservation_t *dhcp_leases_at(int index);

// Drops every reservation.
void dhcp_leases_clear(void);
