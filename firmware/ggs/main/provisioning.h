#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "sb_config.h"

// Runtime configuration, loaded from NVS with the SB_*_DEFAULT values from
// sb_config.h as the fallback on first boot. Set from the configuration
// portal, no recompiling needed.
typedef struct {
    char ap_ssid[33];
    char ap_pass[65];
    int  ap_channel;
    char ap_ip[16];
    int  ap_prefix;      // hotspot netmask length, 8..30 (default 24)
    bool ap_hidden;      // hotspot SSID not broadcast

    char sta_ssid[33];
    char sta_pass[65];

    char ha_mqtt_uri[96];
    char ha_mqtt_user[32];
    char ha_mqtt_pass[32];

    char device_id[32];

    // Name the bridge itself is shown under in Home Assistant. Only the
    // display name: the device identifier stays "spiderbridge", so a
    // rename updates the existing device instead of creating a new one.
    char bridge_name[33];

    // Remote syslog: every system log line is also sent to this host as an
    // RFC 5424 message. Empty disables it.
    //
    // syslog_proto: 0 UDP (RFC 5426), 1 TCP (RFC 6587, octet counting),
    // 2 TLS (RFC 5425). With TLS, syslog_tls_insecure accepts any server
    // certificate; otherwise the CA stored under its own NVS key (see
    // sb_prov_syslog_ca_*) is used, falling back to the built-in bundle of
    // public CAs.
    char syslog_host[64];
    int  syslog_port;
    int  syslog_proto;
    bool syslog_tls_insecure;
    // MQTT log copied into the syslog stream as well: 0 off, 1 raw
    // controller traffic only, 2 raw plus Home Assistant traffic.
    int  syslog_mqtt;

    // SB_PUB_HA, SB_PUB_RAW or SB_PUB_BOTH
    int publish_mode;

    // Empty disables the login. Otherwise the web interface asks for
    // user "admin" and this password.
    char admin_pass[33];

    // --- Internet access ---
    bool wan_open;        // NAT routing for hotspot clients
    bool cloud_forward;   // mirror the controller session to the vendor cloud

    // --- Static address on the home network, empty IP means DHCP ---
    char static_ip[16];
    char static_gw[16];
    char static_mask[16];

    // --- Name and time service for hotspot clients ---
    char dns_target[64];
    bool dns_redirect;
    char ntp_target[64];
    bool ntp_redirect;

    // --- Narrow exceptions while internet access is off ---
    bool allow_dns_offline;
    bool allow_ntp_offline;

    // --- Updates from a URL ---
    //
    // ota_url points at either a firmware .bin or a small JSON manifest
    // describing one. With a manifest the bridge can compare versions
    // before downloading several hundred kilobytes.
    //
    // ota_check enables the periodic check. ota_auto additionally
    // installs what it finds; left off, a newer version is only reported
    // in the interface and installed when the user says so.
    // --- Broker TLS ---
    //
    // Only consulted for an mqtts:// URI. Skips certificate verification
    // entirely, which is what most self-signed home brokers need: the
    // connection is still encrypted, but nothing proves the server is
    // who it claims to be.
    //
    // The CA certificate itself is deliberately NOT in this struct. At
    // 2 KB it would more than double the struct, and this struct is
    // declared on the stack in a dozen places across the firmware —
    // exactly the pattern that caused a boot loop once already. It lives
    // in its own NVS key instead, read only by the MQTT client that
    // needs it. See sb_prov_ca_cert_*().
    bool mqtt_tls_insecure;

    char ota_url[160];
    bool ota_check;
    bool ota_auto;

    // --- Clock ---
    //
    // ntp_server is the time source the bridge itself uses, separate
    // from ntp_target above, which is what hotspot clients are redirected
    // to.
    //
    // tz holds a POSIX TZ string such as "CET-1CEST,M3.5.0,M10.5.0/3".
    // That form carries the switch dates, so daylight saving follows
    // automatically. For a fixed offset with no switching, a string
    // without the comma part is used, e.g. "CET-1".
    char ntp_server[64];
    char tz[48];
    // 0 automatic (the TZ rules decide), 1 force standard time,
    // 2 force daylight saving.
    int  dst_mode;

    // When set, every controller is given the bridge's zone and daylight
    // saving choice as soon as it connects, and again whenever they
    // change here. Left off, each controller keeps its own setting and
    // can be adjusted individually on the Control page.
    //
    // Worth having because schedule times are local: a controller in a
    // different zone from the bridge shows times that look wrong without
    // anything obviously being broken.
    bool tz_push;
    char tz_name[32];   // "Europe/Berlin", the label the controller shows
} sb_prov_cfg_t;

// Convenience helpers for the publish mode.
static inline bool sb_publishes_ha(const sb_prov_cfg_t *c)
{
    return (c->publish_mode & SB_PUB_HA) != 0;
}

static inline bool sb_publishes_raw(const sb_prov_cfg_t *c)
{
    return (c->publish_mode & SB_PUB_RAW) != 0;
}

// Loads the stored configuration, filling in compile-time defaults for
// anything never saved.
void sb_prov_load(sb_prov_cfg_t *out);

// ---------------------------------------------------------------------------
// Broker CA certificate
//
// Kept outside sb_prov_cfg_t because it is large and rarely needed: the
// struct is placed on the stack throughout the firmware, and a 2 KB
// member there is how a stack overflow gets introduced quietly.
//
// Loads into caller-supplied storage. Returns the length, or 0 when no
// certificate is stored.
size_t sb_prov_ca_cert_load(char *out, size_t out_sz);

// Stores a PEM certificate. Pass NULL or an empty string to remove it.
void sb_prov_ca_cert_save(const char *pem);

// Length of the stored certificate without loading it, for the UI.
size_t sb_prov_ca_cert_size(void);

// Same, for the syslog server's CA certificate.
size_t sb_prov_syslog_ca_load(char *out, size_t out_sz);
void   sb_prov_syslog_ca_save(const char *pem);
size_t sb_prov_syslog_ca_size(void);

// ---------------------------------------------------------------------------
// Clock settings, read and written without the whole struct
//
// Several callers need only these, and loading a kilobyte of config to
// read one integer is wasteful on a device this size.
// ---------------------------------------------------------------------------
const char *prov_tz_name(void);   // "Europe/Berlin"
int  prov_dst_mode(void);         // SB_DST_AUTO / STANDARD / SUMMER
bool prov_tz_push(void);          // apply the bridge's clock to controllers

// Stores the daylight-saving choice and applies it to the bridge clock.
void prov_set_dst_mode(int mode);

// Home Assistant display name of the bridge. Never empty: falls back to
// "SpiderBridge". Cached; prov_set_bridge_name() stores and updates it.
const char *prov_bridge_name(void);
void prov_set_bridge_name(const char *name);   // "" = back to the default

// Default bridge name, "SpiderBridge A1B2C3" (end of the hotspot MAC).
void prov_default_bridge_name(char *out, size_t n);

// True when the bridge uses its default name (nothing set by the user).
bool prov_bridge_name_is_default(void);

// Persists the configuration to NVS.
void sb_prov_save(const sb_prov_cfg_t *cfg);

// Writes a fresh random hotspot password to storage (15 characters:
// letters, digits, '#' and '!'). Called by the factory reset so no two
// bridges share the built-in default.
void sb_prov_new_ap_pass(void);

// Generates a random hotspot password into out (15 characters plus the
// terminator, so n must be at least 16). Does not store anything.
void sb_prov_random_ap_pass(char *out, size_t n);

// One-shot "quick connect" switch, set by the web installer. While it is set,
// the next start-up Bluetooth scan also sends this bridge's hotspot Wi-Fi to
// every GGS controller it finds (and leaves them visible to phones). It is
// cleared at the start of that scan, so a failure can never repeat on its own.
bool sb_prov_auto_ble(void);
void sb_prov_set_auto_ble(bool on);

// Stores only the hotspot password (8..63 characters, WPA2). The hotspot
// picks it up on the next start. Returns false for an invalid password or
// a storage error.
bool sb_prov_store_ap_pass(const char *pw);

// Same, but only when no hotspot password is stored yet (first boot after
// flashing an erased chip). Call once after nvs_flash_init().
void sb_prov_ensure_ap_pass(void);

// Hotspot subnet as "192.168.10.0/24".
void sb_prov_ap_subnet_str(const sb_prov_cfg_t *c, char *out, size_t n);

// Parses "a.b.c.d/len" (any host bits allowed, any prefix 8..30). On
// success sets ap_ip to the first usable address and ap_prefix; returns
// false with cfg unchanged for anything malformed.
bool sb_prov_set_ap_subnet(sb_prov_cfg_t *c, const char *cidr);

// Netmask for the stored prefix, as "255.255.255.0".
void sb_prov_ap_netmask_str(const sb_prov_cfg_t *c, char *out, size_t n);
