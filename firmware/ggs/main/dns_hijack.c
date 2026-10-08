#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netdb.h>

#include "lwip/inet.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sb_config.h"
#include "wifi_apsta.h"
#include "wan_gate.h"
#include "dns_hijack.h"

static const char *TAG = "dns_hijack";

#define DNS_PORT        53
#define DNS_BUF_SIZE    512
#define FALLBACK_DNS_IP "8.8.8.8"

// ---------------------------------------------------------------------------
// Runtime settings, all cached here rather than read from NVS per query.
// ---------------------------------------------------------------------------
static uint32_t s_dns_target_ip = 0;     // 0 = follow the DHCP lease
static char     s_dns_target[64] = "";

// When set, every client lookup is forwarded to the configured resolver,
// ignoring whatever DHCP handed out. Lookups are always FORWARDED and the
// real answer relayed back — the bridge never answers on the resolver's
// behalf. Only the one cloud hostname is answered locally, because that
// redirect is what makes the proxy work at all.
static bool     s_dns_force_all = false;

static uint32_t s_ntp_target_ip = 0;
static char     s_ntp_target[64] = "";
static bool     s_ntp_redirect = false;

static bool s_allow_dns_offline = SB_ALLOW_DNS_OFFLINE_DEFAULT;
static bool s_allow_ntp_offline = SB_ALLOW_NTP_OFFLINE_DEFAULT;

// Hostnames treated as time servers. Substring match, so "0.pool.ntp.org"
// and "time.google.com" are both caught.
static const char *NTP_PATTERNS[] = { "ntp", "time", "chrony" };
#define NTP_PATTERN_COUNT (sizeof(NTP_PATTERNS) / sizeof(NTP_PATTERNS[0]))

static bool looks_like_ntp(const char *host)
{
    for (size_t i = 0; i < NTP_PATTERN_COUNT; i++) {
        if (strcasestr(host, NTP_PATTERNS[i])) return true;
    }
    return false;
}

void napt_set_enabled(bool enabled)
{
    if (!g_ap_netif) return;

    esp_err_t err = enabled ? esp_netif_napt_enable(g_ap_netif)
                            : esp_netif_napt_disable(g_ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not %s NAT: %s",
                 enabled ? "enable" : "disable", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "NAT %s", enabled
             ? "enabled — the controller can reach the internet"
             : "disabled — the controller is cut off from the internet");
}

// Reads the resolver the uplink's DHCP lease handed us. Looked up per
// query so a renewed lease takes effect without a restart.
static uint32_t dhcp_resolver(void)
{
    if (!g_sta_netif) return 0;
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(g_sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        return 0;
    }
    if (dns.ip.type != ESP_IPADDR_TYPE_V4) return 0;
    return dns.ip.u_addr.ip4.addr;
}

// Resolves a configured target. An IP literal is used directly; a
// hostname is looked up once over the uplink and the result cached.
static uint32_t resolve_target(const char *target)
{
    if (!target || !target[0]) return 0;

    struct in_addr literal;
    if (inet_pton(AF_INET, target, &literal) == 1) {
        return literal.s_addr;
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    uint32_t ip = 0;
    if (getaddrinfo(target, NULL, &hints, &res) == 0 && res) {
        ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    } else {
        ESP_LOGW(TAG, "Could not resolve '%s'", target);
    }
    if (res) freeaddrinfo(res);
    return ip;
}

void dns_set_target(const char *target, bool force_all)
{
    s_dns_target[0] = '\0';
    if (target && target[0]) {
        strncpy(s_dns_target, target, sizeof(s_dns_target) - 1);
    }
    s_dns_target_ip = resolve_target(s_dns_target);

    // Forcing "whatever DHCP said" is meaningless, so the flag only has
    // an effect once an explicit address is configured.
    s_dns_force_all = force_all && s_dns_target_ip != 0;

    if (!s_dns_target[0]) {
        ESP_LOGI(TAG, "DNS: forwarding lookups to the resolver from DHCP");
    } else if (s_dns_force_all) {
        ESP_LOGI(TAG, "DNS: forwarding every client lookup to %s", s_dns_target);
    } else {
        ESP_LOGI(TAG, "DNS: forwarding lookups to %s", s_dns_target);
    }
}

void dns_set_ntp_target(const char *target, bool enabled)
{
    s_ntp_target[0] = '\0';
    if (target && target[0]) {
        strncpy(s_ntp_target, target, sizeof(s_ntp_target) - 1);
    }
    s_ntp_target_ip = resolve_target(s_ntp_target);
    s_ntp_redirect = enabled && s_ntp_target_ip != 0;

    ESP_LOGI(TAG, "NTP redirection %s%s%s",
             s_ntp_redirect ? "to " : "off",
             s_ntp_redirect ? s_ntp_target : "",
             s_ntp_redirect ? "" : " — clients reach their own time server");
}

void dns_set_offline_exceptions(bool allow_dns, bool allow_ntp)
{
    s_allow_dns_offline = allow_dns;
    s_allow_ntp_offline = allow_ntp;
    ESP_LOGI(TAG, "While offline: DNS %s, NTP %s",
             allow_dns ? "allowed" : "blocked",
             allow_ntp ? "allowed" : "blocked");
}

void dns_effective_resolver(char *out, size_t out_sz)
{
    if (!out || out_sz < 8) return;

    if (s_dns_target_ip) {
        snprintf(out, out_sz, IPSTR, IP2STR((esp_ip4_addr_t *)&s_dns_target_ip));
        return;
    }
    uint32_t ip = dhcp_resolver();
    if (ip) {
        snprintf(out, out_sz, IPSTR " (DHCP)", IP2STR((esp_ip4_addr_t *)&ip));
    } else {
        snprintf(out, out_sz, FALLBACK_DNS_IP " (fallback)");
    }
}

// ---------------------------------------------------------------------------
// Packet handling
// ---------------------------------------------------------------------------

// Returns the offset just past the terminating zero of the question
// name, i.e. where QTYPE begins.
static int dns_extract_qname(const uint8_t *pkt, int len, char *out, int out_sz)
{
    if (len < 12) return -1;
    int pos = 12;
    int o = 0;
    while (pos < len && pkt[pos] != 0) {
        int label_len = pkt[pos++];
        if (label_len > 63 || pos + label_len > len) return -1;
        if (o + label_len + 1 >= out_sz) return -1;
        if (o > 0) out[o++] = '.';
        memcpy(out + o, pkt + pos, label_len);
        o += label_len;
        pos += label_len;
    }
    out[o] = '\0';
    return pos + 1;
}

// Builds an A-record response.
//
// name_end is where QTYPE starts, so the question section only ends four
// bytes later — QTYPE and QCLASS belong to it and MUST be echoed back.
// Copying only up to name_end produces a reply whose answer section
// starts four bytes early: the client reads 0xC00C where it expects
// QTYPE, discards the packet as malformed and retries forever. That bug
// made the controller look like it was ignoring the hijack, and it is
// why dnsmasq worked where an earlier version of this did not.
static void build_a_response(const uint8_t *pkt, int name_end, uint32_t ip_be,
                             uint8_t *out, int *out_len)
{
    int question_end = name_end + 4;

    memcpy(out, pkt, question_end);
    out[2] = 0x81; out[3] = 0x80;               // response, no error
    out[6] = 0x00; out[7] = 0x01;               // ANCOUNT = 1
    out[8] = 0x00; out[9] = 0x00;
    out[10] = 0x00; out[11] = 0x00;

    int pos = question_end;
    out[pos++] = 0xC0; out[pos++] = 0x0C;       // pointer to the question name
    out[pos++] = 0x00; out[pos++] = 0x01;       // TYPE A
    out[pos++] = 0x00; out[pos++] = 0x01;       // CLASS IN
    out[pos++] = 0x00; out[pos++] = 0x00;
    out[pos++] = 0x00; out[pos++] = 0x3C;       // TTL 60 s
    out[pos++] = 0x00; out[pos++] = 0x04;       // RDLENGTH 4
    memcpy(out + pos, &ip_be, 4);
    pos += 4;
    *out_len = pos;
}

static void dns_proxy_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Could not create the DNS socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Could not bind DNS port 53 — is another DNS service running?");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    esp_netif_ip_info_t ap_ip;
    esp_netif_get_ip_info(g_ap_netif, &ap_ip);

    ESP_LOGI(TAG, "DNS proxy listening on port 53 (intercepts '%s')",
             SB_SF_CLOUD_HOST);

    uint8_t buf[DNS_BUF_SIZE];
    uint8_t resp[DNS_BUF_SIZE];
    char qname[256];

    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (len <= 0) continue;

        int qend = dns_extract_qname(buf, len, qname, sizeof(qname));
        if (qend < 0) continue;

        uint16_t qtype = 0;
        if (qend + 2 <= len) {
            qtype = (uint16_t)((buf[qend] << 8) | buf[qend + 1]);
        }

        // --- The one hostname that makes the bridge work ---
        if (strcasecmp(qname, SB_SF_CLOUD_HOST) == 0) {
            // An AAAA query cannot be answered with an IPv4 address. An
            // empty NOERROR tells the client there is no IPv6 record, so
            // it falls back to the A query instead of discarding a
            // mismatched reply.
            if (qtype == 28) {
                memcpy(resp, buf, len > DNS_BUF_SIZE ? DNS_BUF_SIZE : len);
                resp[2] = 0x81; resp[3] = 0x80;
                resp[6] = 0x00; resp[7] = 0x00;
                sendto(sock, resp, len, 0, (struct sockaddr *)&from, fromlen);
                continue;
            }
            int rlen = 0;
            build_a_response(buf, qend, ap_ip.ip.addr, resp, &rlen);
            sendto(sock, resp, rlen, 0, (struct sockaddr *)&from, fromlen);
            ESP_LOGI(TAG, "DNS hijack: %s -> " IPSTR, qname, IP2STR(&ap_ip.ip));
            continue;
        }

        bool is_ntp = looks_like_ntp(qname);

        // --- Exceptions while internet access is off ---
        if (!wan_gate_is_open()) {
            bool permitted = is_ntp ? s_allow_ntp_offline : s_allow_dns_offline;
            if (!permitted) {
                // NXDOMAIN rather than silence, so the client fails fast
                // instead of retrying for seconds.
                memcpy(resp, buf, len > DNS_BUF_SIZE ? DNS_BUF_SIZE : len);
                resp[2] = 0x81; resp[3] = 0x83;
                resp[6] = 0x00; resp[7] = 0x00;
                sendto(sock, resp, len, 0, (struct sockaddr *)&from, fromlen);
                continue;
            }
        }

        // --- Time server redirection ---
        //
        // This one IS answered locally, on purpose: the point is to send
        // the client to a different time server than the one it asked
        // for, which cannot be done by forwarding the query.
        if (s_ntp_redirect && is_ntp && qtype != 28) {
            int rlen = 0;
            build_a_response(buf, qend, s_ntp_target_ip, resp, &rlen);
            sendto(sock, resp, rlen, 0, (struct sockaddr *)&from, fromlen);
            ESP_LOGI(TAG, "NTP redirect: %s -> %s", qname, s_ntp_target);
            continue;
        }

        // --- Suppress record types the bridge cannot route ---
        //
        // The bridge routes IPv4 only. Forwarding an AAAA query returns
        // real IPv6 addresses the client then tries first, because every
        // modern OS prefers IPv6. Those attempts cannot succeed, so the
        // client waits for a connect timeout and only then falls back to
        // IPv4 — per hostname, and a single page pulls in a dozen.
        //
        // Measured here: bulk throughput fine, a page still crawling.
        //
        // An empty NOERROR is the correct answer: it means "this name has
        // no IPv6 address", so the client goes straight to IPv4 with no
        // waiting. NXDOMAIN would be wrong, since the name does exist.
        //
        // Type 65 (HTTPS/SVCB) carries IPv6 hints and HTTP/3 parameters
        // in the same way, so it gets the same treatment.
        if (qtype == 28 || qtype == 65) {
            int rl = len > DNS_BUF_SIZE ? DNS_BUF_SIZE : len;
            memcpy(resp, buf, rl);
            resp[2] = 0x81; resp[3] = 0x80;   // response, NOERROR
            resp[6] = 0x00; resp[7] = 0x00;   // ANCOUNT = 0
            resp[8] = 0x00; resp[9] = 0x00;   // NSCOUNT = 0
            resp[10] = 0x00; resp[11] = 0x00; // ARCOUNT = 0
            sendto(sock, resp, rl, 0, (struct sockaddr *)&from, fromlen);
            continue;
        }

        // --- Forward everything else to a real resolver ---
        //
        // Always a genuine forward, never a synthesised answer: the
        // client gets the resolver's own reply, with all its records and
        // the correct TTL. Only the cloud hostname above is answered
        // locally, because that one redirect is the whole point of the
        // bridge.
        struct sockaddr_in up = {
            .sin_family = AF_INET,
            .sin_port = htons(DNS_PORT),
        };
        if (s_dns_target_ip) {
            // Either an explicit resolver, or the same one forced for
            // every client because "use this for all lookups" is set.
            up.sin_addr.s_addr = s_dns_target_ip;
        } else {
            uint32_t dhcp_ip = dhcp_resolver();
            if (dhcp_ip) {
                up.sin_addr.s_addr = dhcp_ip;
            } else {
                inet_pton(AF_INET, FALLBACK_DNS_IP, &up.sin_addr);
            }
        }

        int usock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (usock < 0) continue;
        struct timeval tv = { .tv_sec = 2 };
        setsockopt(usock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sendto(usock, buf, len, 0, (struct sockaddr *)&up, sizeof(up));
        int ulen = recv(usock, resp, sizeof(resp), 0);
        if (ulen > 0) {
            sendto(sock, resp, ulen, 0, (struct sockaddr *)&from, fromlen);
        }
        close(usock);
    }
}

void dns_hijack_start(void)
{
    // Two 512-byte packet buffers plus a 256-byte hostname buffer, and
    // lwIP's socket calls add their own frames on top. 4096 was measured
    // to overflow; 6144 leaves headroom.
    xTaskCreate(dns_proxy_task, "dns_hijack", 6144, NULL, 5, NULL);
}
