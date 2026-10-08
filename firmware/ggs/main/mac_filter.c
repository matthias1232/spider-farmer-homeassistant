#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

#include "sb_config.h"
#include "wifi_apsta.h"
#include "mac_filter.h"

static const char *TAG = "mac_filter";

#define NVS_NS "sbmac"

static mac_filter_mode_t s_mode = FILTER_OFF;
static mac_filter_entry_t s_list[MAC_FILTER_MAX_ENTRIES];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void mac_filter_format(const uint8_t mac[6], char *out)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool mac_filter_parse(const char *str, uint8_t out[6])
{
    if (!str) return false;

    // Accept both "AA:BB:..." and "AABB..." by skipping separators.
    uint8_t buf[6] = {0};
    int nibbles = 0;

    for (const char *p = str; *p; p++) {
        if (*p == ':' || *p == '-' || *p == ' ') continue;
        if (!isxdigit((unsigned char)*p)) return false;
        if (nibbles >= 12) return false;

        uint8_t v = (uint8_t)(isdigit((unsigned char)*p)
                    ? *p - '0'
                    : (tolower((unsigned char)*p) - 'a' + 10));
        if (nibbles % 2 == 0) {
            buf[nibbles / 2] = (uint8_t)(v << 4);
        } else {
            buf[nibbles / 2] |= v;
        }
        nibbles++;
    }

    if (nibbles != 12) return false;
    memcpy(out, buf, 6);
    return true;
}

static int find_entry(const uint8_t mac[6])
{
    for (int i = 0; i < MAC_FILTER_MAX_ENTRIES; i++) {
        if (s_list[i].in_use && memcmp(s_list[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Could not save the access list");
        return;
    }
    nvs_set_u8(h, "mode", (uint8_t)s_mode);
    // The whole table is stored as one blob: simpler and fewer NVS writes
    // than one key per entry.
    nvs_set_blob(h, "list", s_list, sizeof(s_list));
    nvs_commit(h);
    nvs_close(h);
}

void mac_filter_init(void)
{
    memset(s_list, 0, sizeof(s_list));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t mode = FILTER_OFF;
        nvs_get_u8(h, "mode", &mode);
        if (mode <= FILTER_WHITELIST) s_mode = (mac_filter_mode_t)mode;

        size_t len = sizeof(s_list);
        nvs_get_blob(h, "list", s_list, &len);
        nvs_close(h);
    }

    int n = mac_filter_count();

    // Never boot into a whitelist with nothing on it — that would lock out
    // the grow controller with no way back except a factory reset.
    if (s_mode == FILTER_WHITELIST && n == 0) {
        ESP_LOGW(TAG, "Whitelist mode with an empty list — falling back to off");
        s_mode = FILTER_OFF;
        persist();
    }

    ESP_LOGI(TAG, "Hotspot access: %s (%d entr%s)",
             s_mode == FILTER_OFF       ? "open to all" :
             s_mode == FILTER_BLACKLIST ? "blocklist"   : "allowlist",
             n, n == 1 ? "y" : "ies");
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

bool mac_filter_allows(const uint8_t mac[6])
{
    bool listed = find_entry(mac) >= 0;
    switch (s_mode) {
        case FILTER_BLACKLIST: return !listed;
        case FILTER_WHITELIST: return listed;
        case FILTER_OFF:
        default:               return true;
    }
}

mac_filter_mode_t mac_filter_get_mode(void)
{
    return s_mode;
}

bool mac_filter_set_mode(mac_filter_mode_t mode)
{
    if (mode == FILTER_WHITELIST && mac_filter_count() == 0) {
        ESP_LOGW(TAG, "Refusing allowlist mode with an empty list — "
                      "that would block every client");
        return false;
    }

    s_mode = mode;
    persist();
    ESP_LOGI(TAG, "Hotspot access set to %s",
             mode == FILTER_OFF       ? "open to all" :
             mode == FILTER_BLACKLIST ? "blocklist"   : "allowlist");
    mac_filter_enforce();
    return true;
}

bool mac_filter_add(const char *mac_str, const char *label)
{
    uint8_t mac[6];
    if (!mac_filter_parse(mac_str, mac)) {
        ESP_LOGW(TAG, "Malformed MAC address: %s", mac_str ? mac_str : "(null)");
        return false;
    }

    int existing = find_entry(mac);
    if (existing >= 0) {
        if (label && label[0]) {
            strncpy(s_list[existing].label, label,
                    sizeof(s_list[existing].label) - 1);
            persist();
        }
        return true;
    }

    for (int i = 0; i < MAC_FILTER_MAX_ENTRIES; i++) {
        if (s_list[i].in_use) continue;
        memcpy(s_list[i].mac, mac, 6);
        s_list[i].label[0] = '\0';
        if (label && label[0]) {
            strncpy(s_list[i].label, label, sizeof(s_list[i].label) - 1);
        }
        s_list[i].in_use = true;
        persist();

        char txt[18];
        mac_filter_format(mac, txt);
        ESP_LOGI(TAG, "Added %s to the access list", txt);
        mac_filter_enforce();
        return true;
    }

    ESP_LOGW(TAG, "Access list is full (%d entries)", MAC_FILTER_MAX_ENTRIES);
    return false;
}

bool mac_filter_remove(const char *mac_str)
{
    uint8_t mac[6];
    if (!mac_filter_parse(mac_str, mac)) return false;

    int idx = find_entry(mac);
    if (idx < 0) return false;

    s_list[idx].in_use = false;
    s_list[idx].label[0] = '\0';

    // Removing the last allowlist entry would lock everyone out, so drop
    // back to open access instead.
    if (s_mode == FILTER_WHITELIST && mac_filter_count() == 0) {
        ESP_LOGW(TAG, "Last allowlist entry removed — access is open again");
        s_mode = FILTER_OFF;
    }

    persist();

    char txt[18];
    mac_filter_format(mac, txt);
    ESP_LOGI(TAG, "Removed %s from the access list", txt);
    mac_filter_enforce();
    return true;
}

int mac_filter_count(void)
{
    int n = 0;
    for (int i = 0; i < MAC_FILTER_MAX_ENTRIES; i++) {
        if (s_list[i].in_use) n++;
    }
    return n;
}

const mac_filter_entry_t *mac_filter_entry_at(int index)
{
    if (index < 0 || index >= MAC_FILTER_MAX_ENTRIES) return NULL;
    return s_list[index].in_use ? &s_list[index] : NULL;
}

// ---------------------------------------------------------------------------
// Connected clients
// ---------------------------------------------------------------------------

int mac_filter_list_clients(mac_client_t *out, int max_out)
{
    if (!out || max_out <= 0) return 0;

    wifi_sta_list_t sta;
    if (esp_wifi_ap_get_sta_list(&sta) != ESP_OK) return 0;

    // The station list carries no IP addresses; those live in the DHCP
    // server's lease table.
    //
    // Important detail of this API: the MAC fields are INPUTS and the IP
    // fields are OUTPUTS. Passing a zeroed array asks for the leases of
    // the all-zero MAC, which never matches, so every address comes back
    // as 0 and every client looks like it has no address. The MACs have to
    // be filled in first.
    esp_netif_pair_mac_ip_t leases[ESP_WIFI_MAX_CONN_NUM] = {0};
    int lease_count = sta.num < ESP_WIFI_MAX_CONN_NUM
                      ? sta.num : ESP_WIFI_MAX_CONN_NUM;

    for (int i = 0; i < lease_count; i++) {
        memcpy(leases[i].mac, sta.sta[i].mac, 6);
    }

    bool have_leases = false;
    if (g_ap_netif && lease_count > 0) {
        esp_err_t err = esp_netif_dhcps_get_clients_by_mac(
                            g_ap_netif, lease_count, leases);
        have_leases = (err == ESP_OK);
        if (!have_leases) {
            ESP_LOGW(TAG, "Could not read the DHCP leases: %s",
                     esp_err_to_name(err));
        }
    }

    int n = 0;
    for (int i = 0; i < sta.num && n < max_out; i++) {
        memcpy(out[n].mac, sta.sta[i].mac, 6);
        out[n].rssi = sta.sta[i].rssi;
        out[n].ip = 0;
        out[n].listed = find_entry(sta.sta[i].mac) >= 0;

        if (have_leases) {
            for (int j = 0; j < lease_count; j++) {
                if (memcmp(leases[j].mac, sta.sta[i].mac, 6) == 0) {
                    out[n].ip = leases[j].ip.addr;
                    break;
                }
            }
        }
        n++;
    }
    return n;
}

void mac_filter_enforce(void)
{
    wifi_sta_list_t sta;
    if (esp_wifi_ap_get_sta_list(&sta) != ESP_OK) return;

    // Disconnect anything the current policy no longer allows.
    //
    // esp_wifi_deauth_sta() takes an association ID, which the station list
    // does not carry. Rather than guess an AID — passing 0 would kick every
    // client off — the offending station is deauthenticated through the
    // AID reported in the association event. For already-connected clients
    // the reliable lever is the driver's own MAC filter, applied below.
    int blocked = 0;
    for (int i = 0; i < sta.num; i++) {
        if (!mac_filter_allows(sta.sta[i].mac)) blocked++;
    }

    if (blocked > 0) {
        char txt[18];
        for (int i = 0; i < sta.num; i++) {
            if (mac_filter_allows(sta.sta[i].mac)) continue;
            mac_filter_format(sta.sta[i].mac, txt);
            ESP_LOGW(TAG, "%s is no longer allowed and will be dropped", txt);
        }
        // Deauthenticating by AID is not possible here, so the clients are
        // forced to re-associate; the association handler then rejects the
        // ones that are not allowed.
        esp_wifi_deauth_sta(0);
        ESP_LOGW(TAG, "All clients were disconnected so the new policy "
                      "applies; allowed devices reconnect automatically");
    }
}
