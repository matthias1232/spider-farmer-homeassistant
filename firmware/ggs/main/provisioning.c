#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sb_config.h"
#include "provisioning.h"

static const char *TAG = "provisioning";
#define NVS_NS "sbcfg"

static void get_str(nvs_handle_t h, const char *key, char *out, size_t out_sz,
                    const char *def)
{
    size_t len = out_sz;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        strncpy(out, def, out_sz - 1);
        out[out_sz - 1] = '\0';
    }
}

void sb_prov_load(sb_prov_cfg_t *out)
{
    memset(out, 0, sizeof(*out));

    strncpy(out->ap_ssid, SB_AP_SSID_DEFAULT, sizeof(out->ap_ssid) - 1);
    strncpy(out->ap_pass, SB_AP_PASS_DEFAULT, sizeof(out->ap_pass) - 1);
    out->ap_channel = SB_AP_CHANNEL_DEFAULT;
    strncpy(out->ap_ip, SB_AP_IP_DEFAULT, sizeof(out->ap_ip) - 1);
    out->ap_prefix = 24;
    strncpy(out->sta_ssid, SB_STA_SSID_DEFAULT, sizeof(out->sta_ssid) - 1);
    strncpy(out->sta_pass, SB_STA_PASS_DEFAULT, sizeof(out->sta_pass) - 1);
    strncpy(out->ha_mqtt_uri, SB_HA_MQTT_URI_DEFAULT, sizeof(out->ha_mqtt_uri) - 1);
    strncpy(out->ha_mqtt_user, SB_HA_MQTT_USER_DEFAULT, sizeof(out->ha_mqtt_user) - 1);
    strncpy(out->ha_mqtt_pass, SB_HA_MQTT_PASS_DEFAULT, sizeof(out->ha_mqtt_pass) - 1);
    strncpy(out->device_id, SB_DEVICE_ID_DEFAULT, sizeof(out->device_id) - 1);
    prov_default_bridge_name(out->bridge_name, sizeof(out->bridge_name));
    out->syslog_port = 514;
    out->syslog_proto = 0;
    out->syslog_tls_insecure = false;
    out->publish_mode = SB_PUBLISH_MODE_DEFAULT;
    strncpy(out->admin_pass, SB_ADMIN_PASS_DEFAULT, sizeof(out->admin_pass) - 1);
    out->wan_open = SB_WAN_OPEN_DEFAULT;
    out->cloud_forward = SB_CLOUD_FORWARD_DEFAULT;
    strncpy(out->static_ip, SB_STATIC_IP_DEFAULT, sizeof(out->static_ip) - 1);
    strncpy(out->static_gw, SB_STATIC_GW_DEFAULT, sizeof(out->static_gw) - 1);
    strncpy(out->static_mask, SB_STATIC_MASK_DEFAULT, sizeof(out->static_mask) - 1);
    strncpy(out->dns_target, SB_DNS_TARGET_DEFAULT, sizeof(out->dns_target) - 1);
    out->dns_redirect = SB_DNS_REDIRECT_DEFAULT;
    strncpy(out->ntp_target, SB_NTP_TARGET_DEFAULT, sizeof(out->ntp_target) - 1);
    out->ntp_redirect = SB_NTP_REDIRECT_DEFAULT;
    out->allow_dns_offline = SB_ALLOW_DNS_OFFLINE_DEFAULT;
    out->allow_ntp_offline = SB_ALLOW_NTP_OFFLINE_DEFAULT;
    out->mqtt_tls_insecure = false;
    strncpy(out->ntp_server, SB_NTP_SERVER_DEFAULT, sizeof(out->ntp_server) - 1);
    strncpy(out->tz, SB_TZ_DEFAULT, sizeof(out->tz) - 1);
    out->dst_mode = SB_DST_MODE_DEFAULT;
    out->tz_push = SB_TZ_PUSH_DEFAULT;
    strncpy(out->tz_name, SB_TZ_NAME_DEFAULT, sizeof(out->tz_name) - 1);
    strncpy(out->ota_url, SB_OTA_URL_DEFAULT, sizeof(out->ota_url) - 1);
    out->ota_check = SB_OTA_CHECK_DEFAULT;
    out->ota_auto = SB_OTA_AUTO_DEFAULT;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No stored configuration yet — using compile-time defaults");
        return;
    }

    get_str(h, "ap_ssid", out->ap_ssid, sizeof(out->ap_ssid), SB_AP_SSID_DEFAULT);
    get_str(h, "ap_pass", out->ap_pass, sizeof(out->ap_pass), SB_AP_PASS_DEFAULT);
    get_str(h, "ap_ip", out->ap_ip, sizeof(out->ap_ip), SB_AP_IP_DEFAULT);
    uint8_t apx = 24;
    nvs_get_u8(h, "ap_prefix", &apx);
    out->ap_prefix = (apx >= 8 && apx <= 30) ? apx : 24;
    uint8_t aph = 0;
    nvs_get_u8(h, "ap_hidden", &aph);
    out->ap_hidden = aph != 0;

    int32_t ch = SB_AP_CHANNEL_DEFAULT;
    nvs_get_i32(h, "ap_channel", &ch);
    out->ap_channel = (int)ch;

    get_str(h, "sta_ssid", out->sta_ssid, sizeof(out->sta_ssid), SB_STA_SSID_DEFAULT);
    get_str(h, "sta_pass", out->sta_pass, sizeof(out->sta_pass), SB_STA_PASS_DEFAULT);
    get_str(h, "ha_uri", out->ha_mqtt_uri, sizeof(out->ha_mqtt_uri), SB_HA_MQTT_URI_DEFAULT);
    get_str(h, "ha_user", out->ha_mqtt_user, sizeof(out->ha_mqtt_user), SB_HA_MQTT_USER_DEFAULT);
    get_str(h, "ha_pass", out->ha_mqtt_pass, sizeof(out->ha_mqtt_pass), SB_HA_MQTT_PASS_DEFAULT);
    get_str(h, "device_id", out->device_id, sizeof(out->device_id), SB_DEVICE_ID_DEFAULT);
    get_str(h, "br_name", out->bridge_name, sizeof(out->bridge_name), "");
    if (!out->bridge_name[0] || strcmp(out->bridge_name, "SpiderBridge") == 0)
        prov_default_bridge_name(out->bridge_name, sizeof(out->bridge_name));
    get_str(h, "slog_host", out->syslog_host, sizeof(out->syslog_host), "");
    int32_t sp = 514;
    nvs_get_i32(h, "slog_port", &sp);
    out->syslog_port = (sp > 0 && sp < 65536) ? (int)sp : 514;
    uint8_t sproto = 0;
    nvs_get_u8(h, "slog_proto", &sproto);
    out->syslog_proto = sproto <= 2 ? sproto : 0;
    uint8_t sins = 0;
    nvs_get_u8(h, "slog_ins", &sins);
    out->syslog_tls_insecure = sins != 0;
    uint8_t smq = 0;
    nvs_get_u8(h, "slog_mqtt", &smq);
    out->syslog_mqtt = smq <= 2 ? smq : 0;
    get_str(h, "admin_pass", out->admin_pass, sizeof(out->admin_pass), SB_ADMIN_PASS_DEFAULT);

    uint8_t mode = SB_PUBLISH_MODE_DEFAULT;
    nvs_get_u8(h, "pubmode", &mode);
    if (mode < SB_PUB_HA || mode > SB_PUB_BOTH) mode = SB_PUBLISH_MODE_DEFAULT;
    out->publish_mode = mode;

    get_str(h, "sip", out->static_ip, sizeof(out->static_ip), SB_STATIC_IP_DEFAULT);
    get_str(h, "sgw", out->static_gw, sizeof(out->static_gw), SB_STATIC_GW_DEFAULT);
    get_str(h, "smask", out->static_mask, sizeof(out->static_mask), SB_STATIC_MASK_DEFAULT);
    get_str(h, "dns_tgt", out->dns_target, sizeof(out->dns_target), SB_DNS_TARGET_DEFAULT);
    get_str(h, "ntp_tgt", out->ntp_target, sizeof(out->ntp_target), SB_NTP_TARGET_DEFAULT);

    uint8_t flag;
    flag = SB_WAN_OPEN_DEFAULT ? 1 : 0;
    nvs_get_u8(h, "wan", &flag);           out->wan_open = flag != 0;
    flag = SB_CLOUD_FORWARD_DEFAULT ? 1 : 0;
    nvs_get_u8(h, "cloudfw", &flag);       out->cloud_forward = flag != 0;
    flag = SB_DNS_REDIRECT_DEFAULT ? 1 : 0;
    nvs_get_u8(h, "dns_rd", &flag);        out->dns_redirect = flag != 0;
    flag = SB_NTP_REDIRECT_DEFAULT ? 1 : 0;
    nvs_get_u8(h, "ntp_rd", &flag);        out->ntp_redirect = flag != 0;
    flag = SB_ALLOW_DNS_OFFLINE_DEFAULT ? 1 : 0;
    nvs_get_u8(h, "dns_ex", &flag);        out->allow_dns_offline = flag != 0;
    flag = SB_ALLOW_NTP_OFFLINE_DEFAULT ? 1 : 0;
    nvs_get_u8(h, "ntp_ex", &flag);        out->allow_ntp_offline = flag != 0;

    flag = 0;
    nvs_get_u8(h, "mqtt_ins", &flag);      out->mqtt_tls_insecure = flag != 0;

    get_str(h, "ntp_srv", out->ntp_server, sizeof(out->ntp_server), SB_NTP_SERVER_DEFAULT);
    get_str(h, "tz", out->tz, sizeof(out->tz), SB_TZ_DEFAULT);
    int32_t dm = SB_DST_MODE_DEFAULT;
    nvs_get_i32(h, "dst_mode", &dm);
    out->dst_mode = (int)dm;

    flag = SB_TZ_PUSH_DEFAULT ? 1 : 0;
    nvs_get_u8(h, "tz_push", &flag);       out->tz_push = flag != 0;
    get_str(h, "tz_name", out->tz_name, sizeof(out->tz_name), SB_TZ_NAME_DEFAULT);

    get_str(h, "ota_url", out->ota_url, sizeof(out->ota_url), SB_OTA_URL_DEFAULT);
    flag = SB_OTA_CHECK_DEFAULT ? 1 : 0;
    nvs_get_u8(h, "ota_chk", &flag);       out->ota_check = flag != 0;
    flag = SB_OTA_AUTO_DEFAULT ? 1 : 0;
    nvs_get_u8(h, "ota_auto", &flag);      out->ota_auto = flag != 0;

    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Broker CA certificate
//
// Stored under its own key so the large payload never travels inside
// sb_prov_cfg_t, which is routinely placed on a task stack.
// ---------------------------------------------------------------------------

size_t sb_prov_ca_cert_load(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;

    size_t len = out_sz;
    esp_err_t err = nvs_get_str(h, "mqtt_ca", out, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        out[0] = '\0';
        return 0;
    }
    return strlen(out);
}

void sb_prov_ca_cert_save(const char *pem)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    if (pem && pem[0]) {
        nvs_set_str(h, "mqtt_ca", pem);
    } else {
        nvs_erase_key(h, "mqtt_ca");
    }
    nvs_commit(h);
    nvs_close(h);
}

// --- Syslog server CA, under its own key for the same reason as above ---

size_t sb_prov_syslog_ca_load(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = out_sz;
    esp_err_t err = nvs_get_str(h, "slog_ca", out, &len);
    nvs_close(h);
    if (err != ESP_OK) { out[0] = '\0'; return 0; }
    return strlen(out);
}

void sb_prov_syslog_ca_save(const char *pem)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (pem && pem[0]) nvs_set_str(h, "slog_ca", pem);
    else               nvs_erase_key(h, "slog_ca");
    nvs_commit(h);
    nvs_close(h);
}

size_t sb_prov_syslog_ca_size(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, "slog_ca", NULL, &len);
    nvs_close(h);
    return (err == ESP_OK && len > 1) ? len - 1 : 0;
}

size_t sb_prov_ca_cert_size(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;

    size_t len = 0;
    esp_err_t err = nvs_get_str(h, "mqtt_ca", NULL, &len);
    nvs_close(h);

    return (err == ESP_OK && len > 1) ? len - 1 : 0;
}

// ---------------------------------------------------------------------------
// Clock settings, read without the whole struct
//
// Cached after the first read: the status page asks for these every few
// seconds and an NVS read per poll is needless work.
// ---------------------------------------------------------------------------

static char s_tz_name_cache[32] = "";
static int  s_dst_cache = -1;
static int  s_push_cache = -1;

static void clock_cache_load(void)
{
    if (s_dst_cache >= 0) return;

    nvs_handle_t h;
    s_dst_cache = SB_DST_MODE_DEFAULT;
    s_push_cache = SB_TZ_PUSH_DEFAULT ? 1 : 0;
    strncpy(s_tz_name_cache, SB_TZ_NAME_DEFAULT, sizeof(s_tz_name_cache) - 1);

    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    int32_t dm = s_dst_cache;
    if (nvs_get_i32(h, "dst_mode", &dm) == ESP_OK) s_dst_cache = (int)dm;

    uint8_t flag = (uint8_t)s_push_cache;
    if (nvs_get_u8(h, "tz_push", &flag) == ESP_OK) s_push_cache = flag ? 1 : 0;

    size_t len = sizeof(s_tz_name_cache);
    nvs_get_str(h, "tz_name", s_tz_name_cache, &len);

    nvs_close(h);
}

const char *prov_tz_name(void)
{
    clock_cache_load();
    return s_tz_name_cache;
}

int prov_dst_mode(void)
{
    clock_cache_load();
    return s_dst_cache;
}

bool prov_tz_push(void)
{
    clock_cache_load();
    return s_push_cache != 0;
}

void prov_set_dst_mode(int mode)
{
    if (mode < SB_DST_AUTO || mode > SB_DST_SUMMER) return;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "dst_mode", mode);
        nvs_commit(h);
        nvs_close(h);
    }
    clock_cache_load();
    s_dst_cache = mode;
}

// ---------------------------------------------------------------------------
// Bridge display name
// ---------------------------------------------------------------------------

static char s_bridge_name[33] = "";

// "SpiderBridge 5E823D": the last three bytes of the bridge's hotspot MAC,
// so several bridges in one Home Assistant stay apart without naming them.
void prov_default_bridge_name(char *out, size_t n)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        snprintf(out, n, "SpiderBridge");
        return;
    }
    snprintf(out, n, "SpiderBridge %02X%02X%02X", mac[3], mac[4], mac[5]);
}

const char *prov_bridge_name(void)
{
    if (!s_bridge_name[0]) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(s_bridge_name);
            if (nvs_get_str(h, "br_name", s_bridge_name, &len) != ESP_OK) s_bridge_name[0] = '\0';
            nvs_close(h);
        }
        // Not set, or the old plain default: the default with the MAC.
        if (!s_bridge_name[0] || strcmp(s_bridge_name, "SpiderBridge") == 0)
            prov_default_bridge_name(s_bridge_name, sizeof(s_bridge_name));
    }
    return s_bridge_name;
}

bool prov_bridge_name_is_default(void)
{
    char def[33];
    prov_default_bridge_name(def, sizeof(def));
    return strcmp(prov_bridge_name(), def) == 0;
}

void prov_set_bridge_name(const char *name)
{
    char clean[33] = "";
    if (name) {
        // Trimmed and without characters that would break the hand-built
        // JSON the bridge entities are announced with.
        size_t n = 0;
        while (*name == ' ') name++;
        for (; *name && n < sizeof(clean) - 1; name++) {
            if (*name == '"' || *name == '\\' || (unsigned char)*name < 0x20) continue;
            clean[n++] = *name;
        }
        while (n > 0 && clean[n - 1] == ' ') clean[--n] = '\0';
    }
    // Empty means "use the default": stored as empty, so the default (with
    // the MAC) is what is shown and announced.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "br_name", clean);
        nvs_commit(h);
        nvs_close(h);
    }
    if (clean[0]) strcpy(s_bridge_name, clean);
    else prov_default_bridge_name(s_bridge_name, sizeof(s_bridge_name));
}

// Invalidates the cache after a full save.
static void clock_cache_clear(void) { s_dst_cache = -1; s_bridge_name[0] = '\0'; }

void sb_prov_save(const sb_prov_cfg_t *cfg)
{
    // The clock values may have changed, so the cache must be re-read.
    clock_cache_clear();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS to save the configuration");
        return;
    }

    nvs_set_str(h, "ap_ssid", cfg->ap_ssid);
    nvs_set_str(h, "ap_pass", cfg->ap_pass);
    nvs_set_str(h, "ap_ip", cfg->ap_ip);
    nvs_set_u8(h, "ap_prefix", (uint8_t)(cfg->ap_prefix >= 8 && cfg->ap_prefix <= 30 ? cfg->ap_prefix : 24));
    nvs_set_u8(h, "ap_hidden", cfg->ap_hidden ? 1 : 0);
    nvs_set_i32(h, "ap_channel", cfg->ap_channel);
    nvs_set_str(h, "sta_ssid", cfg->sta_ssid);
    nvs_set_str(h, "sta_pass", cfg->sta_pass);
    nvs_set_str(h, "ha_uri", cfg->ha_mqtt_uri);
    nvs_set_str(h, "ha_user", cfg->ha_mqtt_user);
    nvs_set_str(h, "ha_pass", cfg->ha_mqtt_pass);
    nvs_set_str(h, "device_id", cfg->device_id);
    {
        // The default is not stored: an empty field keeps following it.
        char def[33];
        prov_default_bridge_name(def, sizeof(def));
        bool is_default = !cfg->bridge_name[0] || strcmp(cfg->bridge_name, def) == 0 ||
                          strcmp(cfg->bridge_name, "SpiderBridge") == 0;
        nvs_set_str(h, "br_name", is_default ? "" : cfg->bridge_name);
    }
    nvs_set_str(h, "slog_host", cfg->syslog_host);
    nvs_set_i32(h, "slog_port", cfg->syslog_port > 0 ? cfg->syslog_port : 514);
    nvs_set_u8(h, "slog_proto", (uint8_t)(cfg->syslog_proto <= 2 ? cfg->syslog_proto : 0));
    nvs_set_u8(h, "slog_ins", cfg->syslog_tls_insecure ? 1 : 0);
    nvs_set_u8(h, "slog_mqtt", (uint8_t)(cfg->syslog_mqtt >= 0 && cfg->syslog_mqtt <= 2 ? cfg->syslog_mqtt : 0));
    nvs_set_str(h, "admin_pass", cfg->admin_pass);
    nvs_set_u8(h, "pubmode", (uint8_t)cfg->publish_mode);

    nvs_set_u8(h, "wan", cfg->wan_open ? 1 : 0);
    nvs_set_u8(h, "cloudfw", cfg->cloud_forward ? 1 : 0);
    nvs_set_str(h, "sip", cfg->static_ip);
    nvs_set_str(h, "sgw", cfg->static_gw);
    nvs_set_str(h, "smask", cfg->static_mask);
    nvs_set_str(h, "dns_tgt", cfg->dns_target);
    nvs_set_u8(h, "dns_rd", cfg->dns_redirect ? 1 : 0);
    nvs_set_str(h, "ntp_tgt", cfg->ntp_target);
    nvs_set_u8(h, "ntp_rd", cfg->ntp_redirect ? 1 : 0);
    nvs_set_u8(h, "dns_ex", cfg->allow_dns_offline ? 1 : 0);
    nvs_set_u8(h, "ntp_ex", cfg->allow_ntp_offline ? 1 : 0);
    nvs_set_u8(h, "mqtt_ins", cfg->mqtt_tls_insecure ? 1 : 0);
    nvs_set_str(h, "ntp_srv", cfg->ntp_server);
    nvs_set_str(h, "tz", cfg->tz);
    nvs_set_i32(h, "dst_mode", cfg->dst_mode);
    nvs_set_u8(h, "tz_push", cfg->tz_push ? 1 : 0);
    nvs_set_str(h, "tz_name", cfg->tz_name);
    nvs_set_str(h, "ota_url", cfg->ota_url);
    nvs_set_u8(h, "ota_chk", cfg->ota_check ? 1 : 0);
    nvs_set_u8(h, "ota_auto", cfg->ota_auto ? 1 : 0);

    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "Configuration saved");
}

static bool parse_ip4(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

static uint32_t prefix_mask(int len) { return len <= 0 ? 0 : 0xFFFFFFFFu << (32 - len); }

void sb_prov_ap_subnet_str(const sb_prov_cfg_t *c, char *out, size_t n)
{
    uint32_t ip = 0;
    int len = (c->ap_prefix >= 8 && c->ap_prefix <= 30) ? c->ap_prefix : 24;
    if (!parse_ip4(c->ap_ip, &ip)) parse_ip4(SB_AP_IP_DEFAULT, &ip);
    uint32_t net = ip & prefix_mask(len);
    snprintf(out, n, "%u.%u.%u.%u/%d", (unsigned)(net >> 24), (unsigned)(net >> 16) & 255,
             (unsigned)(net >> 8) & 255, (unsigned)net & 255, len);
}

void sb_prov_ap_netmask_str(const sb_prov_cfg_t *c, char *out, size_t n)
{
    int len = (c->ap_prefix >= 8 && c->ap_prefix <= 30) ? c->ap_prefix : 24;
    uint32_t m = prefix_mask(len);
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)(m >> 24), (unsigned)(m >> 16) & 255,
             (unsigned)(m >> 8) & 255, (unsigned)m & 255);
}

bool sb_prov_set_ap_subnet(sb_prov_cfg_t *c, const char *cidr)
{
    char buf[24];
    strncpy(buf, cidr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *slash = strchr(buf, '/');
    int len = 24;
    if (slash) {
        *slash = '\0';
        char *end;
        long l = strtol(slash + 1, &end, 10);
        if (*end || l < 8 || l > 30) return false;   // /31, /32 leave no client address
        len = (int)l;
    }
    uint32_t ip;
    if (!parse_ip4(buf, &ip)) return false;
    uint32_t net = ip & prefix_mask(len);
    uint32_t first = net + 1;
    // 0.x, 127.x, multicast and above are not usable as a hotspot.
    unsigned top = net >> 24;
    if (top == 0 || top == 127 || top >= 224) return false;
    snprintf(c->ap_ip, sizeof(c->ap_ip), "%u.%u.%u.%u", (unsigned)(first >> 24),
             (unsigned)(first >> 16) & 255, (unsigned)(first >> 8) & 255, (unsigned)first & 255);
    c->ap_prefix = len;
    return true;
}

void sb_prov_ensure_ap_pass(void)
{
    nvs_handle_t h;
    size_t len = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t e = nvs_get_str(h, "ap_pass", NULL, &len);
        nvs_close(h);
        if (e == ESP_OK && len > 1) return;
    }
    sb_prov_new_ap_pass();
}

void sb_prov_new_ap_pass(void)
{
    // 15 characters from 64 symbols = 90 bits: the same shape as the
    // password set by hand on the reference bridge. esp_random() is a
    // hardware RNG once the radio has run; the bootloader seeds it too.
    static const char SET[] =
        "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789#!";
    char pw[16];
    const size_t n = sizeof(SET) - 1;
    bool lo, up, dg, sy;
    do {
        lo = up = dg = sy = false;
        for (int i = 0; i < 15; i++) {
            char c = SET[esp_random() % n];
            pw[i] = c;
            if (c >= 'a' && c <= 'z') lo = true;
            else if (c >= 'A' && c <= 'Z') up = true;
            else if (c >= '0' && c <= '9') dg = true;
            else sy = true;
        }
        pw[15] = '\0';
    } while (!(lo && up && dg && sy));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ap_pass", pw);
    nvs_commit(h);
    nvs_close(h);
    memset(pw, 0, sizeof(pw));
    ESP_LOGW(TAG, "New hotspot password generated -- shown on the settings page");
}
