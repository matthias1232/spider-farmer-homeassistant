#pragma once
#include <stdbool.h>
#include "esp_netif.h"

// Sets up the AP (hotspot for the controller) and, if a home network SSID
// is configured, the STA uplink. Safe to call with no STA credentials: the
// AP still comes up, the STA side simply stays disabled.
void wifi_apsta_init(void);

// Blocks until the STA interface has an IP, or the timeout elapses.
// Returns false immediately if STA was never enabled (no SSID configured).
bool wifi_apsta_wait_uplink(uint32_t timeout_ms);

// true while the uplink currently has an address.
bool wifi_apsta_uplink_is_up(void);

// Starts the connection watchdog. It nudges the Wi-Fi driver while the
// uplink is down and restarts the device if it stays down for too long,
// so an outage that outlasts the driver's own retries still heals.
void wifi_apsta_start_watchdog(void);

// ---------------------------------------------------------------------------
// Status, for the web interface
//
// What someone needs to diagnose a misbehaving bridge without a serial
// cable: which network it joined, how good the signal is, which
// addresses it holds, and who is connected to its hotspot.
// ---------------------------------------------------------------------------
typedef struct {
    bool     sta_enabled;      // an uplink SSID is configured at all
    bool     sta_connected;    // associated with the access point
    bool     sta_has_ip;       // association plus a usable address
    char     sta_ssid[33];
    char     sta_bssid[18];    // which access point, when several share an SSID
    int      sta_channel;
    int      sta_rssi;         // dBm, negative; closer to zero is better
    int      sta_quality;      // 0-100, derived from RSSI for readability
    char     sta_ip[16];
    char     sta_netmask[16];
    char     sta_gateway[16];
    char     sta_dns[16];
    uint32_t sta_disconnects;  // since boot; a climbing count means a weak link
    uint32_t sta_last_down_s;  // seconds since the uplink was last lost

    char     ap_ssid[33];
    char     ap_ip[16];
    int      ap_channel;
    int      ap_clients;       // stations currently associated
    char     ap_mac[18];
} wifi_status_t;

// Fills in the current status. Always succeeds; fields that do not apply
// are left empty or zero.
void wifi_apsta_get_status(wifi_status_t *out);

// ---------------------------------------------------------------------------
// Hotspot client connect times
//
// The Wi-Fi driver reports who is associated but not since when, so the
// join time is recorded from the association event.
// ---------------------------------------------------------------------------
void wifi_clients_note_join(const uint8_t mac[6]);
void wifi_clients_note_leave(const uint8_t mac[6]);

// Seconds since that client associated, 0 when it is not connected.
uint32_t wifi_clients_connected_for(const uint8_t mac[6]);

// ---------------------------------------------------------------------------
// Home network scan and live connect (Settings page)
// ---------------------------------------------------------------------------
typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    uint8_t auth;      // wifi_auth_mode_t
} wifi_scan_ap_t;

// Scans for networks (blocking, ~2-4 s; the hotspot stays up). Returns the
// number found (strongest first, one entry per SSID), at most max.
int wifi_apsta_scan(wifi_scan_ap_t *out, int max);

// Joins ssid/pass now, without a restart. Every step goes to the log
// (and so to syslog). Returns true when an address was obtained within
// timeout_ms; on failure the previous network is joined again.
bool wifi_apsta_connect_now(const char *ssid, const char *pass, uint32_t timeout_ms,
                            char *msg, size_t msgsz);

// Exposed so other modules (NAPT, DHCP lease lookups) can use the netif
// handles directly instead of looking them up again.
extern esp_netif_t *g_ap_netif;
extern esp_netif_t *g_sta_netif;
