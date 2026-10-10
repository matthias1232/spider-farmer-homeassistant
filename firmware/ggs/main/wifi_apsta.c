#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_wifi.h"
#include "supervisor.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "config_poll.h"
#include "lwip/inet.h"
#include "dhcpserver/dhcpserver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_timer.h"

#include "sb_config.h"
#include "provisioning.h"
#include "mac_filter.h"
#include "wifi_apsta.h"

static const char *TAG = "wifi_apsta";

esp_netif_t *g_ap_netif = NULL;
esp_netif_t *g_sta_netif = NULL;

static EventGroupHandle_t s_wifi_evt;
#define STA_CONNECTED_BIT BIT0

// Without a configured home network the STA side must not try to connect
// at all — otherwise esp_wifi_connect() spins in a loop with no SSID to
// aim at, burning CPU and keeping the radio busy enough that the AP side
// becomes sluggish.
static bool s_sta_enabled = false;

// Tick count when the uplink went down, 0 while it is up. The watchdog
// below uses it to escalate from plain retries to a reboot.
static TickType_t s_uplink_down_since = 0;
static int s_reconnect_attempts = 0;
static uint32_t s_sta_disconnects = 0;

// Last channel the radio was seen on, so a change can be noticed while
// the uplink stays connected. Zero until the first check.
static uint8_t s_last_known_channel = 0;

// When each hotspot client joined.
//
// The Wi-Fi driver reports who is associated but not since when, so the
// join time is recorded from the association event. A small fixed table:
// clients come and go, and a growing list would be a slow leak.
#define CLIENT_SLOTS 8
typedef struct {
    bool     in_use;
    uint8_t  mac[6];
    uint32_t since_s;    // seconds of uptime at association
} client_slot_t;

static client_slot_t s_clients[CLIENT_SLOTS];

static uint32_t uptime_now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

void wifi_clients_note_join(const uint8_t mac[6])
{
    for (int i = 0; i < CLIENT_SLOTS; i++) {
        if (s_clients[i].in_use && memcmp(s_clients[i].mac, mac, 6) == 0) {
            s_clients[i].since_s = uptime_now_s();
            return;
        }
    }
    for (int i = 0; i < CLIENT_SLOTS; i++) {
        if (s_clients[i].in_use) continue;
        s_clients[i].in_use = true;
        memcpy(s_clients[i].mac, mac, 6);
        s_clients[i].since_s = uptime_now_s();
        return;
    }
}

void wifi_clients_note_leave(const uint8_t mac[6])
{
    for (int i = 0; i < CLIENT_SLOTS; i++) {
        if (s_clients[i].in_use && memcmp(s_clients[i].mac, mac, 6) == 0) {
            s_clients[i].in_use = false;
            return;
        }
    }
}

uint32_t wifi_clients_connected_for(const uint8_t mac[6])
{
    for (int i = 0; i < CLIENT_SLOTS; i++) {
        if (s_clients[i].in_use && memcmp(s_clients[i].mac, mac, 6) == 0) {
            uint32_t now = uptime_now_s();
            return now > s_clients[i].since_s ? now - s_clients[i].since_s : 0;
        }
    }
    return 0;
}

static volatile int s_last_disc_reason = 0;

static const char *disc_reason_text(int r)
{
    switch (r) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "wrong password";
        case WIFI_REASON_NO_AP_FOUND:            return "network not found";
        case WIFI_REASON_ASSOC_FAIL:             return "association failed";
        case WIFI_REASON_BEACON_TIMEOUT:         return "signal lost";
        case WIFI_REASON_AUTH_EXPIRE:            return "authentication expired";
        default:                                 return "see Wi-Fi reason codes";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_enabled) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *c = (wifi_event_sta_connected_t *)data;
        ESP_LOGI(TAG, "Uplink: associated with \"%.*s\" (channel %d), waiting for an address",
                 c->ssid_len, (const char *)c->ssid, c->channel);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_last_disc_reason = d->reason;
        if (!s_sta_enabled) return;
        ESP_LOGW(TAG, "Uplink: disconnected from \"%.*s\", reason %d (%s)",
                 d->ssid_len, (const char *)d->ssid, d->reason, disc_reason_text(d->reason));
        xEventGroupClearBits(s_wifi_evt, STA_CONNECTED_BIT);
        // Counted for the status page: a number that keeps climbing is
        // the clearest sign of a marginal uplink, even when the bridge
        // looks fine at the moment someone checks it.
        s_sta_disconnects++;
        // The retry itself happens on the watchdog task. Sleeping here
        // would block the event loop, which also delivers the AP-side
        // events the hotspot depends on.
        s_uplink_down_since = s_uplink_down_since ? s_uplink_down_since
                                                  : xTaskGetTickCount();
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Uplink connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_uplink_down_since = 0;
        s_reconnect_attempts = 0;
        xEventGroupSetBits(s_wifi_evt, STA_CONNECTED_BIT);

        // Keep the stored hotspot channel honest.
        //
        // One radio serves both roles, so the AP physically cannot stay
        // on a different channel from the uplink — the driver silently
        // retunes it and leaves the saved setting behind. Measured on a
        // live device: the config said channel 6, a scan from another
        // machine found the hotspot on channel 1.
        //
        // Writing the real channel back means the stored value matches
        // reality, the status page stops reporting a channel the radio
        // is not on, and the next boot brings the AP up on the right
        // channel immediately instead of starting on one and being
        // dragged to another once the uplink associates. That retune at
        // boot is itself disruptive to any hotspot client that
        // connected in the meantime — including the controller.
        //
        // Only written when it actually changed: this fires on every
        // reconnect, and NVS should not be rewritten for no reason.
        uint8_t ch = 0;
        wifi_second_chan_t sec;
        if (esp_wifi_get_channel(&ch, &sec) == ESP_OK && ch) {
            sb_prov_cfg_t *p = malloc(sizeof(*p));
            if (p) {
                sb_prov_load(p);
                if (p->ap_channel != ch) {
                    ESP_LOGI(TAG, "Hotspot channel follows the uplink: "
                                  "%d -> %d (saved)", p->ap_channel, ch);
                    p->ap_channel = ch;
                    sb_prov_save(p);
                }
                free(p);
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;

        // Access check at the moment of association.
        //
        // The event carries the association ID, which is what
        // esp_wifi_deauth_sta() needs. The v1 firmware had to work from
        // the station list, which does not carry it, and fell back to
        // deauth_sta(0) — disconnecting every client to remove one.
        if (!mac_filter_allows(e->mac)) {
            ESP_LOGW(TAG, "Refused " MACSTR " — not permitted on the hotspot",
                     MAC2STR(e->mac));
            esp_wifi_deauth_sta(e->aid);
            return;
        }

        ESP_LOGI(TAG, "Client joined the hotspot, MAC=" MACSTR, MAC2STR(e->mac));
        wifi_clients_note_join(e->mac);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "Client left the hotspot, MAC=" MACSTR, MAC2STR(e->mac));
        wifi_clients_note_leave(e->mac);
    }
}

void wifi_apsta_init(void)
{
    s_wifi_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    g_ap_netif = esp_netif_create_default_wifi_ap();
    g_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // On the heap: wifi_config_t and esp_netif_ip_info_t already sit on
    // this stack, and the driver calls below go several frames deep. One
    // more kilobyte-sized struct here is what tipped the main task over
    // once the OTA and clock fields enlarged the configuration.
    sb_prov_cfg_t *prov = malloc(sizeof(*prov));
    if (!prov) {
        ESP_LOGE(TAG, "Out of memory loading the Wi-Fi configuration");
        return;
    }
    sb_prov_load(prov);

    s_sta_enabled = (prov->sta_ssid[0] != '\0');

    // Only APSTA when a real uplink is wanted. In pure AP mode the driver
    // never searches for a station to join, which keeps the AP responsive
    // instead of periodically stalling on "Haven't to connect to a
    // suitable AP now!" scans.
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_sta_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP));

    // Address the AP interface before starting its DHCP server, so the
    // handed-out leases match the configured gateway.
    esp_netif_dhcps_stop(g_ap_netif);
    esp_netif_ip_info_t ip_info;
    esp_netif_str_to_ip4(prov->ap_ip, &ip_info.ip);
    esp_netif_str_to_ip4(prov->ap_ip, &ip_info.gw);
    // Any prefix 8..30 from the settings ("Hotspot subnet").
    char mask[16];
    sb_prov_ap_netmask_str(prov, mask, sizeof(mask));
    esp_netif_str_to_ip4(mask, &ip_info.netmask);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(g_ap_netif, &ip_info));

    // Pool: the addresses after the bridge's own, up to the broadcast
    // address, capped at the DHCP server's lease table (DHCPS_MAX_LEASE,
    // 100). A /30 gives one client, a /24 gives 100, a /16 still 100.
    {
        uint32_t ip = ntohl(ip_info.ip.addr), m = ntohl(ip_info.netmask.addr);
        uint32_t first = ip + 1, last = (ip & m) | ~m;
        if (last > first) last -= 1;                 // skip broadcast
        if (last - first + 1 > 100) last = first + 99;
        dhcps_lease_t lease = { .enable = true };
        lease.start_ip.addr = htonl(first);
        lease.end_ip.addr = htonl(last);
        if (esp_netif_dhcps_option(g_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS,
                                   &lease, sizeof(lease)) != ESP_OK)
            ESP_LOGW(TAG, "Hotspot DHCP pool not set, using the server's default");
        ESP_LOGI(TAG, "Hotspot %s/%s, DHCP pool " IPSTR " - " IPSTR, prov->ap_ip, mask,
                 IP2STR(&lease.start_ip), IP2STR(&lease.end_ip));
    }

    // Tell clients to use the bridge as their resolver. Without this the
    // DHCP server hands out no DNS server at all, so the controller cannot
    // resolve the cloud hostname and the hijack in dns_hijack.c never gets
    // a chance to redirect it to the proxy. Confirmed by testing: without
    // this option the controller gets an address but never queries DNS.
    esp_netif_dns_info_t ap_dns = {0};
    ap_dns.ip.type = ESP_IPADDR_TYPE_V4;
    ap_dns.ip.u_addr.ip4 = ip_info.ip;
    esp_netif_set_dns_info(g_ap_netif, ESP_NETIF_DNS_MAIN, &ap_dns);

    uint8_t offer_dns = 1;
    esp_netif_dhcps_option(g_ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));

    ESP_ERROR_CHECK(esp_netif_dhcps_start(g_ap_netif));

    wifi_config_t ap_config = { 0 };
    strncpy((char *)ap_config.ap.ssid, prov->ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(prov->ap_ssid);
    strncpy((char *)ap_config.ap.password, prov->ap_pass, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.channel = prov->ap_channel;
    ap_config.ap.max_connection = SB_AP_MAX_CONN;
    // Hidden: no SSID in the beacons. Controllers that already know the
    // network keep joining it; it just no longer shows up in scans.
    ap_config.ap.ssid_hidden = prov->ap_hidden ? 1 : 0;
    ap_config.ap.authmode = strlen(prov->ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    // No PMF requirement, otherwise the controller drops the connection
    // after a few hours (confirmed against the known-working hostapd
    // config this replaces).
    ap_config.ap.pmf_cfg.required = false;

    if (s_sta_enabled) {
        // Static address on the home network, when one is configured.
        // Without it the bridge takes whatever DHCP offers, which can
        // change and makes the web interface harder to find once the
        // device is mounted out of reach.
        if (prov->static_ip[0] != '\0') {
            esp_netif_ip_info_t sta_ip = {0};
            if (esp_netif_str_to_ip4(prov->static_ip, &sta_ip.ip) == ESP_OK &&
                esp_netif_str_to_ip4(prov->static_gw, &sta_ip.gw) == ESP_OK &&
                esp_netif_str_to_ip4(prov->static_mask, &sta_ip.netmask) == ESP_OK) {

                esp_netif_dhcpc_stop(g_sta_netif);
                if (esp_netif_set_ip_info(g_sta_netif, &sta_ip) == ESP_OK) {
                    ESP_LOGI(TAG, "Static address %s, gateway %s",
                             prov->static_ip, prov->static_gw);

                    // Without DHCP there is no resolver either, so point
                    // it at the gateway to keep name resolution working.
                    esp_netif_dns_info_t dns = {0};
                    dns.ip.type = ESP_IPADDR_TYPE_V4;
                    dns.ip.u_addr.ip4 = sta_ip.gw;
                    esp_netif_set_dns_info(g_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
                } else {
                    ESP_LOGE(TAG, "Could not apply the static address — "
                                  "falling back to DHCP");
                    esp_netif_dhcpc_start(g_sta_netif);
                }
            } else {
                ESP_LOGE(TAG, "Static address settings are malformed — using DHCP");
            }
        }

        wifi_config_t sta_config = { 0 };
        strncpy((char *)sta_config.sta.ssid, prov->sta_ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, prov->sta_pass, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.pmf_cfg.capable = true;
        sta_config.sta.pmf_cfg.required = false;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Match the access point to what the controller actually tolerates.
    //
    // The hostapd setup this bridge replaces ran with wmm_enabled=0 and
    // hw_mode=g: plain 802.11g, 20 MHz, no QoS, no frame aggregation. The
    // ESP32 defaults to 802.11n with WMM and 40 MHz, which is not
    // cosmetic: with aggregation enabled the controller associates,
    // negotiates a Block-ACK session, and then deauthenticates with
    // reason 8 every few seconds, never getting as far as opening a TCP
    // connection. This was confirmed by packet-level testing against the
    // real controller.
    //
    // 802.11n stays enabled as a capability so ordinary clients (phones,
    // laptops) are not forced down to 802.11g rates. The controller
    // negotiates b/g on its own; capping the whole AP at 11g made every
    // other client share a very slow channel, which showed up as web
    // pages that start loading and then stall.
    esp_err_t pm_err = esp_wifi_set_protocol(WIFI_IF_AP,
                                             WIFI_PROTOCOL_11B |
                                             WIFI_PROTOCOL_11G |
                                             WIFI_PROTOCOL_11N);
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not set the AP protocol set: %s",
                 esp_err_to_name(pm_err));
    }
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);

    // Power save off on the STA side as well. With it enabled the radio
    // sleeps between beacons of the uplink AP, and because AP and STA
    // share one radio, those naps stall traffic for hotspot clients too.
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Report the channel the AP actually ended up on.
    //
    // The ESP32 has a single radio: the AP cannot run on a different
    // channel than the STA uplink. When the configured AP channel differs
    // from the uplink's, the driver silently retunes the AP ("ap channel
    // adjust o:6,0 n:1,0" in the log) and every hotspot client has to
    // share airtime with the uplink traffic. That halves throughput at
    // best and causes visible stalls at worst.
    uint8_t actual_ch = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&actual_ch, &second);

    ESP_LOGI(TAG, "AP SSID='%s' IP=%s", prov->ap_ssid, prov->ap_ip);
    if (s_sta_enabled) {
        ESP_LOGI(TAG, "STA SSID='%s' (uplink)", prov->sta_ssid);
        if (actual_ch && actual_ch != prov->ap_channel) {
            ESP_LOGW(TAG, "AP moved from channel %d to %d to match the uplink",
                     prov->ap_channel, actual_ch);
            ESP_LOGW(TAG, "One radio serves both roles, so hotspot clients "
                          "share airtime with the uplink. Setting the hotspot "
                          "channel to %d in the portal avoids the retune.",
                     actual_ch);
        } else {
            ESP_LOGI(TAG, "AP and uplink share channel %d", actual_ch);
        }
    } else {
        ESP_LOGW(TAG, "STA disabled — no home network configured");
    }

    free(prov);
}

bool wifi_apsta_wait_uplink(uint32_t timeout_ms)
{
    if (!s_sta_enabled) return false;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt, STA_CONNECTED_BIT,
                                            pdFALSE, pdTRUE,
                                            timeout_ms / portTICK_PERIOD_MS);
    return (bits & STA_CONNECTED_BIT) != 0;
}

bool wifi_apsta_uplink_is_up(void)
{
    if (!s_sta_enabled) return false;
    return (xEventGroupGetBits(s_wifi_evt) & STA_CONNECTED_BIT) != 0;
}

// RSSI in dBm is precise but hard to read at a glance. This is the usual
// mapping: -50 and above is excellent, -100 is unusable.
static int rssi_to_quality(int rssi)
{
    if (rssi >= -50) return 100;
    if (rssi <= -100) return 0;
    return 2 * (rssi + 100);
}

void wifi_apsta_get_status(wifi_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->sta_enabled = s_sta_enabled;
    out->sta_has_ip  = wifi_apsta_uplink_is_up();
    out->sta_disconnects = s_sta_disconnects;

    if (s_uplink_down_since) {
        out->sta_last_down_s =
            (uint32_t)((xTaskGetTickCount() - s_uplink_down_since) /
                       configTICK_RATE_HZ);
    }

    // --- Uplink ---
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->sta_connected = true;
        strncpy(out->sta_ssid, (const char *)ap.ssid, sizeof(out->sta_ssid) - 1);
        snprintf(out->sta_bssid, sizeof(out->sta_bssid),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        out->sta_channel = ap.primary;
        out->sta_rssi    = ap.rssi;
        out->sta_quality = rssi_to_quality(ap.rssi);
    }

    if (g_sta_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(g_sta_netif, &ip) == ESP_OK) {
            snprintf(out->sta_ip, sizeof(out->sta_ip), IPSTR, IP2STR(&ip.ip));
            snprintf(out->sta_netmask, sizeof(out->sta_netmask), IPSTR,
                     IP2STR(&ip.netmask));
            snprintf(out->sta_gateway, sizeof(out->sta_gateway), IPSTR,
                     IP2STR(&ip.gw));
        }
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(g_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            snprintf(out->sta_dns, sizeof(out->sta_dns), IPSTR,
                     IP2STR(&dns.ip.u_addr.ip4));
        }
    }

    // --- Hotspot ---
    wifi_config_t apcfg;
    if (esp_wifi_get_config(WIFI_IF_AP, &apcfg) == ESP_OK) {
        strncpy(out->ap_ssid, (const char *)apcfg.ap.ssid, sizeof(out->ap_ssid) - 1);
        out->ap_channel = apcfg.ap.channel;
    }

    // Report the channel the radio is really on, not the one that was
    // asked for.
    //
    // There is one radio for both roles, so the AP cannot sit on a
    // different channel from the uplink: the driver quietly moves it to
    // follow the STA and leaves the stored config untouched. Reporting
    // the stored value meant the status page claimed channel 6 while a
    // scan from another machine showed the hotspot on channel 1 — the
    // kind of detail someone would waste an afternoon on while chasing
    // dropouts.
    uint8_t real_ch = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&real_ch, &second) == ESP_OK && real_ch) {
        out->ap_channel = real_ch;
    }

    if (g_ap_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(g_ap_netif, &ip) == ESP_OK) {
            snprintf(out->ap_ip, sizeof(out->ap_ip), IPSTR, IP2STR(&ip.ip));
        }
    }

    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        snprintf(out->ap_mac, sizeof(out->ap_mac),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    wifi_sta_list_t stations;
    if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK) {
        out->ap_clients = stations.num;
    }
}

// ---------------------------------------------------------------------------
// Connection watchdog
//
// The Wi-Fi driver retries on its own and the MQTT client reconnects by
// itself, so most outages heal without help. This task exists for the
// cases that do not:
//
//   * the driver stops retrying after repeated failures
//   * the router comes back on a different channel or band
//   * some internal state wedges and no event ever fires again
//
// The device is meant to sit unattended next to a grow tent, possibly out
// of reach, so recovering from an hour-long router outage has to be
// automatic. Escalation: keep nudging the driver, and if the uplink is
// still down after SB_UPLINK_REBOOT_AFTER_S, restart. A reboot takes two
// seconds and is far better than staying dark until someone notices.
// ---------------------------------------------------------------------------
// Runs every 10 s on the config-poll task's hook (no task of its own).
static void connection_watchdog(void)
{
    {
        // --- Follow a channel change while still connected ---
        //
        // Routers move channel on their own: auto-selection reacting to
        // interference, a DFS event, or simply a reboot picking
        // differently. The station follows automatically, but the AP
        // half is dragged along with it — one radio cannot do two
        // channels — and everything connected to the hotspot has its
        // link disrupted without anything appearing "down".
        //
        // The rest of this task only looks at a *failed* uplink, so this
        // case went unnoticed: the uplink stays up, the stored AP
        // channel quietly goes stale, and the hotspot ends up described
        // as being on a channel it is not on.
        //
        // Checked while connected, and only written when it actually
        // changed, so this costs one driver call every ten seconds.
        if (s_sta_enabled && s_uplink_down_since == 0) {
            uint8_t ch = 0;
            wifi_second_chan_t sec;
            if (esp_wifi_get_channel(&ch, &sec) == ESP_OK && ch &&
                ch != s_last_known_channel) {
                if (s_last_known_channel) {
                    ESP_LOGW(TAG, "Uplink moved to channel %d (was %d) — "
                                  "the hotspot follows it", ch,
                             s_last_known_channel);
                }
                s_last_known_channel = ch;

                sb_prov_cfg_t *p = malloc(sizeof(*p));
                if (p) {
                    sb_prov_load(p);
                    if (p->ap_channel != ch) {
                        p->ap_channel = ch;
                        sb_prov_save(p);
                        ESP_LOGI(TAG, "Hotspot channel saved as %d", ch);
                    }
                    free(p);
                }
            }
        }

        if (!s_sta_enabled || s_uplink_down_since == 0) return;

        TickType_t down_ticks = xTaskGetTickCount() - s_uplink_down_since;
        int down_seconds = (int)(down_ticks * portTICK_PERIOD_MS / 1000);

        s_reconnect_attempts++;

        if (down_seconds >= SB_UPLINK_REBOOT_AFTER_S) {
            ESP_LOGE(TAG, "Uplink down for %d s after %d attempts — restarting",
                     down_seconds, s_reconnect_attempts);
            vTaskDelay(pdMS_TO_TICKS(500));
            sv_restart(SV_WHY_UPLINK);
        }

        // Every third check, tear the station interface down and bring it
        // back. A plain esp_wifi_connect() is not always enough when the
        // driver has given up internally.
        if (s_reconnect_attempts % 3 == 0) {
            ESP_LOGW(TAG, "Uplink down %d s — cycling the station interface",
                     down_seconds);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            ESP_LOGW(TAG, "Uplink down %d s — retrying", down_seconds);
        }

        esp_wifi_connect();
    }
}

int wifi_apsta_scan(wifi_scan_ap_t *out, int max)
{
    // A scan needs the station interface; in pure AP mode it is switched
    // on for the scan (the hotspot keeps running).
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_LOGI(TAG, "Wi-Fi scan for the home network started");
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                              .scan_time.active = { .min = 80, .max = 200 } };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    int n = 0;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
    } else {
        uint16_t cnt = 0;
        esp_wifi_scan_get_ap_num(&cnt);
        if (cnt > 40) cnt = 40;
        wifi_ap_record_t *rec = calloc(cnt ? cnt : 1, sizeof(*rec));
        if (rec && esp_wifi_scan_get_ap_records(&cnt, rec) == ESP_OK) {
            // Records come strongest first; keep one entry per SSID.
            for (int i = 0; i < cnt && n < max; i++) {
                if (!rec[i].ssid[0]) continue;
                bool dup = false;
                for (int k = 0; k < n && !dup; k++)
                    dup = strcmp(out[k].ssid, (const char *)rec[i].ssid) == 0;
                if (dup) continue;
                strncpy(out[n].ssid, (const char *)rec[i].ssid, sizeof(out[n].ssid) - 1);
                out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
                out[n].rssi = rec[i].rssi;
                out[n].channel = rec[i].primary;
                out[n].auth = rec[i].authmode;
                n++;
            }
        } else {
            esp_wifi_clear_ap_list();
        }
        free(rec);
        ESP_LOGI(TAG, "Wi-Fi scan done: %d network(s)", n);
    }
    if (mode == WIFI_MODE_AP && !s_sta_enabled) esp_wifi_set_mode(WIFI_MODE_AP);
    return n;
}

bool wifi_apsta_connect_now(const char *ssid, const char *pass, uint32_t timeout_ms,
                            char *msg, size_t msgsz)
{
    if (!ssid || !ssid[0]) { snprintf(msg, msgsz, "No network given"); return false; }
    ESP_LOGI(TAG, "Uplink: connecting to \"%s\" now (requested on the settings page)", ssid);

    wifi_config_t old = {0};
    esp_wifi_get_config(WIFI_IF_STA, &old);
    bool had = s_sta_enabled;

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA) {
        ESP_LOGI(TAG, "Uplink: switching the radio to hotspot + station");
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    // Stop the old link first; the disconnect handler must not reconnect
    // to the old network in between.
    s_sta_enabled = false;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_wifi_evt, STA_CONNECTED_BIT);

    wifi_config_t c = {0};
    strncpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid) - 1);
    strncpy((char *)c.sta.password, pass ? pass : "", sizeof(c.sta.password) - 1);
    c.sta.pmf_cfg.capable = true;
    c.sta.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &c);
    if (err != ESP_OK) {
        snprintf(msg, msgsz, "Could not apply the settings (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "Uplink: %s", msg);
        s_sta_enabled = had;
        return false;
    }
    ESP_LOGI(TAG, "Uplink: credentials set (password %u characters), connecting ...",
             (unsigned)strlen(pass ? pass : ""));
    s_last_disc_reason = 0;
    s_sta_enabled = true;
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt, STA_CONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & STA_CONNECTED_BIT) {
        esp_netif_ip_info_t ip = {0};
        esp_netif_get_ip_info(g_sta_netif, &ip);
        snprintf(msg, msgsz, "Connected to \"%s\", address " IPSTR, ssid, IP2STR(&ip.ip));
        ESP_LOGI(TAG, "Uplink: %s", msg);
        return true;
    }

    int r = s_last_disc_reason;
    snprintf(msg, msgsz, "Could not connect to \"%s\"%s%s%s", ssid,
             r ? " (" : " (no address within the time limit",
             r ? disc_reason_text(r) : "", ")");
    ESP_LOGW(TAG, "Uplink: %s -- going back to the previous network", msg);
    s_sta_enabled = false;
    esp_wifi_disconnect();
    if (had && old.sta.ssid[0]) {
        esp_wifi_set_config(WIFI_IF_STA, &old);
        s_sta_enabled = true;
        esp_wifi_connect();
        ESP_LOGI(TAG, "Uplink: reconnecting to \"%s\"", (const char *)old.sta.ssid);
    } else if (!had) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
    return false;
}

void wifi_apsta_start_watchdog(void)
{
    config_poll_add_hook(connection_watchdog, 10);
    ESP_LOGI(TAG, "Connection watchdog started (reboot after %d s offline)",
             SB_UPLINK_REBOOT_AFTER_S);
}
