#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sb_config.h"
#include "wifi_apsta.h"
#include "ip_filter.h"

static const char *TAG = "ip_filter";
#define NVS_NS "sbip"

static ip_filter_mode_t s_mode = IPF_OFF;
static ip_filter_rule_t s_rules[IPF_MAX_RULES];

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Could not save the address rules");
        return;
    }
    nvs_set_u8(h, "mode", (uint8_t)s_mode);
    nvs_set_blob(h, "rules", s_rules, sizeof(s_rules));
    nvs_commit(h);
    nvs_close(h);
}

void ip_filter_init(void)
{
    memset(s_rules, 0, sizeof(s_rules));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t mode = IPF_OFF;
        nvs_get_u8(h, "mode", &mode);
        if (mode <= IPF_ALLOWLIST) s_mode = (ip_filter_mode_t)mode;

        size_t len = sizeof(s_rules);
        nvs_get_blob(h, "rules", s_rules, &len);
        nvs_close(h);
    }

    int n = ip_filter_count();

    // Never boot into an allow-list with nothing on it: that would
    // refuse every request, and the settings page is the only way to
    // undo it.
    if (s_mode == IPF_ALLOWLIST && n == 0) {
        ESP_LOGW(TAG, "Allow-list mode with no rules — falling back to off");
        s_mode = IPF_OFF;
        persist();
    }

    ESP_LOGI(TAG, "Web interface access: %s (%d rule%s)",
             s_mode == IPF_OFF       ? "open to all" :
             s_mode == IPF_BLOCKLIST ? "blocklist"   : "allowlist",
             n, n == 1 ? "" : "s");
}

ip_filter_mode_t ip_filter_get_mode(void) { return s_mode; }

int ip_filter_count(void)
{
    int n = 0;
    for (int i = 0; i < IPF_MAX_RULES; i++) {
        if (s_rules[i].in_use) n++;
    }
    return n;
}

const ip_filter_rule_t *ip_filter_at(int index)
{
    if (index < 0 || index >= IPF_MAX_RULES) return NULL;
    return s_rules[index].in_use ? &s_rules[index] : NULL;
}

bool ip_filter_set_mode(ip_filter_mode_t mode)
{
    if (mode == IPF_ALLOWLIST && ip_filter_count() == 0) {
        ESP_LOGW(TAG, "Refusing allow-list mode with no rules — "
                      "that would block every client");
        return false;
    }
    s_mode = mode;
    persist();
    ESP_LOGI(TAG, "Web interface access set to %s",
             mode == IPF_OFF       ? "open to all" :
             mode == IPF_BLOCKLIST ? "blocklist"   : "allowlist");
    return true;
}

static uint32_t mask_for(uint8_t prefix)
{
    if (prefix == 0) return 0;
    if (prefix >= 32) return 0xFFFFFFFFu;
    return 0xFFFFFFFFu << (32 - prefix);
}

bool ip_filter_add(const char *cidr, const char *note)
{
    if (!cidr || !cidr[0]) return false;

    char work[32];
    strncpy(work, cidr, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    uint8_t prefix = 32;
    char *slash = strchr(work, '/');
    if (slash) {
        *slash = '\0';
        int p = atoi(slash + 1);
        if (p < 0 || p > 32) {
            ESP_LOGW(TAG, "Prefix out of range in '%s'", cidr);
            return false;
        }
        prefix = (uint8_t)p;
    }

    struct in_addr a;
    if (inet_pton(AF_INET, work, &a) != 1) {
        ESP_LOGW(TAG, "Malformed address: %s", work);
        return false;
    }

    uint32_t net = ntohl(a.s_addr) & mask_for(prefix);

    for (int i = 0; i < IPF_MAX_RULES; i++) {
        if (s_rules[i].in_use &&
            s_rules[i].network == net && s_rules[i].prefix == prefix) {
            return true;   // already present
        }
    }

    for (int i = 0; i < IPF_MAX_RULES; i++) {
        if (s_rules[i].in_use) continue;
        s_rules[i].in_use = true;
        s_rules[i].network = net;
        s_rules[i].prefix = prefix;
        s_rules[i].note[0] = '\0';
        if (note && note[0]) {
            strncpy(s_rules[i].note, note, IPF_NOTE_LEN - 1);
        }
        persist();

        char txt[20];
        ip_filter_format(&s_rules[i], txt);
        ESP_LOGI(TAG, "Added %s", txt);
        return true;
    }

    ESP_LOGW(TAG, "Rule list is full (%d entries)", IPF_MAX_RULES);
    return false;
}

bool ip_filter_remove(int index)
{
    if (index < 0 || index >= IPF_MAX_RULES || !s_rules[index].in_use) {
        return false;
    }
    memset(&s_rules[index], 0, sizeof(s_rules[index]));

    // Removing the last allow-list rule would lock everyone out, so drop
    // back to open access rather than leave an unusable state.
    if (s_mode == IPF_ALLOWLIST && ip_filter_count() == 0) {
        ESP_LOGW(TAG, "Last allow-list rule removed — access is open again");
        s_mode = IPF_OFF;
    }

    persist();
    return true;
}

void ip_filter_format(const ip_filter_rule_t *r, char *out)
{
    if (!r || !out) return;
    uint32_t n = r->network;
    snprintf(out, 20, "%u.%u.%u.%u/%u",
             (unsigned)((n >> 24) & 0xFF), (unsigned)((n >> 16) & 0xFF),
             (unsigned)((n >> 8) & 0xFF),  (unsigned)(n & 0xFF),
             (unsigned)r->prefix);
}

static bool matches(uint32_t addr)
{
    for (int i = 0; i < IPF_MAX_RULES; i++) {
        if (!s_rules[i].in_use) continue;
        uint32_t m = mask_for(s_rules[i].prefix);
        if ((addr & m) == s_rules[i].network) return true;
    }
    return false;
}

bool ip_filter_allows_request(httpd_req_t *req)
{
    if (s_mode == IPF_OFF) return true;

    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return true;   // cannot tell; do not lock anyone out

    struct sockaddr_in6 peer;
    socklen_t len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &len) != 0) return true;

    // lwIP hands back IPv4 addresses mapped into IPv6 here.
    uint32_t addr;
    if (peer.sin6_family == AF_INET) {
        addr = ntohl(((struct sockaddr_in *)&peer)->sin_addr.s_addr);
    } else {
        const uint8_t *b = (const uint8_t *)&peer.sin6_addr;
        addr = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
               ((uint32_t)b[14] << 8)  |  (uint32_t)b[15];
    }

    // --- The escape hatch ---
    //
    // Clients on the bridge's own hotspot are always allowed, whatever
    // the rules say. Two filters can lock a user out of this device and
    // the only other way back is a serial cable; joining the hotspot is
    // always possible, so this guarantees a route in.
    if (g_ap_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(g_ap_netif, &ip) == ESP_OK) {
            uint32_t ap_net = ntohl(ip.ip.addr) & ntohl(ip.netmask.addr);
            if ((addr & ntohl(ip.netmask.addr)) == ap_net) return true;
        }
    }

    bool listed = matches(addr);
    bool allowed = (s_mode == IPF_BLOCKLIST) ? !listed : listed;

    if (!allowed) {
        ESP_LOGW(TAG, "Refused %u.%u.%u.%u",
                 (unsigned)((addr >> 24) & 0xFF), (unsigned)((addr >> 16) & 0xFF),
                 (unsigned)((addr >> 8) & 0xFF),  (unsigned)(addr & 0xFF));
    }
    return allowed;
}
