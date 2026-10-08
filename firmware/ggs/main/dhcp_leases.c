#include <string.h>
#include <stdio.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sb_config.h"
#include "wifi_apsta.h"
#include "mac_filter.h"
#include "dhcp_leases.h"

static const char *TAG = "dhcp";

#define NVS_NS "sbdhcp"

static dhcp_reservation_t s_res[DHCP_MAX_RESERVATIONS];

static int find(const uint8_t mac[6])
{
    for (int i = 0; i < DHCP_MAX_RESERVATIONS; i++) {
        if (s_res[i].in_use && memcmp(s_res[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Could not save the reservations");
        return;
    }
    nvs_set_blob(h, "res", s_res, sizeof(s_res));
    nvs_commit(h);
    nvs_close(h);
}

void dhcp_leases_init(void)
{
    memset(s_res, 0, sizeof(s_res));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_res);
        nvs_get_blob(h, "res", s_res, &len);
        nvs_close(h);
    }

    int n = dhcp_leases_count();
    if (n > 0) {
        ESP_LOGI(TAG, "%d named device%s known", n, n == 1 ? "" : "s");
        for (int i = 0; i < DHCP_MAX_RESERVATIONS; i++) {
            if (!s_res[i].in_use) continue;
            char mac_txt[18];
            mac_filter_format(s_res[i].mac, mac_txt);
            if (s_res[i].ip) {
                ESP_LOGI(TAG, "   %s  %-20s reserved " IPSTR,
                         mac_txt, s_res[i].name,
                         IP2STR((esp_ip4_addr_t *)&s_res[i].ip));
            } else {
                ESP_LOGI(TAG, "   %s  %s", mac_txt, s_res[i].name);
            }
        }
    }
}

const char *dhcp_leases_name_for(const uint8_t mac[6])
{
    int i = find(mac);
    return (i >= 0 && s_res[i].name[0]) ? s_res[i].name : NULL;
}

uint32_t dhcp_leases_reserved_ip(const uint8_t mac[6])
{
    int i = find(mac);
    return i >= 0 ? s_res[i].ip : 0;
}

bool dhcp_leases_set(const char *mac_str, const char *name, const char *ip_str)
{
    uint8_t mac[6];
    if (!mac_filter_parse(mac_str, mac)) {
        ESP_LOGW(TAG, "Malformed MAC address: %s", mac_str ? mac_str : "(null)");
        return false;
    }

    uint32_t ip = 0;
    if (ip_str && ip_str[0]) {
        struct in_addr a;
        if (inet_pton(AF_INET, ip_str, &a) != 1) {
            ESP_LOGW(TAG, "Malformed IP address: %s", ip_str);
            return false;
        }
        ip = a.s_addr;

        // A reservation outside the hotspot subnet would never be handed
        // out, so reject it rather than silently ignoring it.
        if (g_ap_netif) {
            esp_netif_ip_info_t info;
            if (esp_netif_get_ip_info(g_ap_netif, &info) == ESP_OK) {
                if ((ip & info.netmask.addr) != (info.ip.addr & info.netmask.addr)) {
                    ESP_LOGW(TAG, "%s is outside the hotspot subnet — "
                                  "reservation refused", ip_str);
                    return false;
                }
            }
        }
    }

    int i = find(mac);
    if (i < 0) {
        for (int j = 0; j < DHCP_MAX_RESERVATIONS; j++) {
            if (!s_res[j].in_use) { i = j; break; }
        }
        if (i < 0) {
            ESP_LOGW(TAG, "No room for more reservations (%d max)",
                     DHCP_MAX_RESERVATIONS);
            return false;
        }
        memcpy(s_res[i].mac, mac, 6);
        s_res[i].in_use = true;
    }

    s_res[i].ip = ip;
    s_res[i].name[0] = '\0';
    if (name && name[0]) {
        strncpy(s_res[i].name, name, DHCP_NAME_LEN - 1);
    }
    persist();

    char mac_txt[18];
    mac_filter_format(mac, mac_txt);
    if (ip) {
        ESP_LOGI(TAG, "%s is '%s', reserved %s", mac_txt, s_res[i].name, ip_str);
    } else {
        ESP_LOGI(TAG, "%s is '%s'", mac_txt, s_res[i].name);
    }
    return true;
}

bool dhcp_leases_remove(const char *mac_str)
{
    uint8_t mac[6];
    if (!mac_filter_parse(mac_str, mac)) return false;

    int i = find(mac);
    if (i < 0) return false;

    memset(&s_res[i], 0, sizeof(s_res[i]));
    persist();
    return true;
}

int dhcp_leases_count(void)
{
    int n = 0;
    for (int i = 0; i < DHCP_MAX_RESERVATIONS; i++) {
        if (s_res[i].in_use) n++;
    }
    return n;
}

const dhcp_reservation_t *dhcp_leases_at(int index)
{
    if (index < 0 || index >= DHCP_MAX_RESERVATIONS) return NULL;
    return s_res[index].in_use ? &s_res[index] : NULL;
}

void dhcp_leases_clear(void)
{
    memset(s_res, 0, sizeof(s_res));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Reservations cleared");
}
