#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_system.h"
#include "supervisor.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sb_config.h"
#include "provisioning.h"
#include "device_registry.h"
#include "web_auth.h"
#include "sb_system.h"

static const char *TAG = "system";

// Every namespace the firmware writes to. Kept in one place so a new
// feature's storage cannot be forgotten by a factory reset.
static const char *NAMESPACES[] = {
    "sbcfg",    // settings
    "sbdev",    // device names
    "sbmac",    // hotspot access list
    "sbdhcp",   // device labels
    "sbip",     // web interface IP rules
};
#define NS_COUNT (sizeof(NAMESPACES) / sizeof(NAMESPACES[0]))

// Backup format version. Bumped when the shape changes, so a restore can
// refuse a file it does not understand instead of importing nonsense.
#define BACKUP_VERSION 1

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((int)(intptr_t)arg));
    device_registry_flush();   // a just-made rename is on flash before the restart
    ESP_LOGI(TAG, "Restarting now");
    sv_restart(SV_WHY_USER);
}

void sb_system_reboot(int delay_ms)
{
    xTaskCreate(reboot_task, "sb_reboot", 2048,
                (void *)(intptr_t)delay_ms, 5, NULL);
}

void sb_system_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset: clearing all stored settings");

    for (size_t i = 0; i < NS_COUNT; i++) {
        nvs_handle_t h;
        if (nvs_open(NAMESPACES[i], NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGI(TAG, "  cleared %s", NAMESPACES[i]);
        }
    }

    ESP_LOGW(TAG, "Restarting unconfigured — the settings page will be at "
                  "http://%s/", SB_AP_IP_DEFAULT);
    sb_system_reboot(1500);
}

// Reads a form field from an application/x-www-form-urlencoded body.
static bool form_field(const char *body, const char *key,
                       char *out, size_t out_sz)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *eq = strchr(p, '=');
        if (eq && (!amp || eq < amp) &&
            (size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            size_t len = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
            if (len >= out_sz) len = out_sz - 1;
            memcpy(out, eq + 1, len);
            out[len] = '\0';
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

esp_err_t system_reset_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    char body[128] = "";
    if (req->content_len > 0 && req->content_len < (int)sizeof(body)) {
        int received = httpd_req_recv(req, body, req->content_len);
        if (received > 0) body[received] = '\0';
    }

    char confirm[32] = "";
    form_field(body, "confirm", confirm, sizeof(confirm));

    if (strcmp(confirm, "RESET") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req,
            "Type RESET to confirm. This clears the WiFi credentials, "
            "the broker settings and every device name.",
            HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, "Cleared. Restarting — the bridge will come back "
                         "unconfigured on its own hotspot.",
                    HTTPD_RESP_USE_STRLEN);

    sb_system_factory_reset();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Backup
// ---------------------------------------------------------------------------

esp_err_t backup_get_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    sb_prov_cfg_t *p = malloc(sizeof(*p));
    char *buf = malloc(1024);
    if (!p || !buf) {
        free(p); free(buf);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Not enough memory right now");
        return ESP_OK;
    }
    sb_prov_load(p);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"spiderbridge-backup.json\"");

    // Streamed in pieces: with the certificate included the whole file
    // is larger than is comfortable to hold at once on this device.
    int n = snprintf(buf, 1024,
        "{\"version\":%d,\"note\":\"Contains WiFi and MQTT passwords. "
        "Keep this file private.\","
        "\"sta_ssid\":\"%s\",\"sta_pass\":\"%s\","
        "\"ap_ssid\":\"%s\",\"ap_pass\":\"%s\","
        "\"ap_ip\":\"%s\",\"ap_prefix\":%d,\"ap_hidden\":%s,\"ap_channel\":%d,",
        BACKUP_VERSION, p->sta_ssid, p->sta_pass,
        p->ap_ssid, p->ap_pass, p->ap_ip, p->ap_prefix,
        p->ap_hidden ? "true" : "false", p->ap_channel);
    httpd_resp_send_chunk(req, buf, n);

    n = snprintf(buf, 1024,
        "\"mqtt_uri\":\"%s\",\"mqtt_user\":\"%s\",\"mqtt_pass\":\"%s\","
        "\"device_id\":\"%s\",\"bridge_name\":\"%s\",\"publish_mode\":%d,"
        "\"syslog_host\":\"%s\",\"syslog_port\":%d,\"syslog_proto\":%d,"
        "\"syslog_tls_insecure\":%s,\"syslog_mqtt\":%d,"
        "\"mqtt_tls_insecure\":%s,",
        p->ha_mqtt_uri, p->ha_mqtt_user, p->ha_mqtt_pass,
        p->device_id, p->bridge_name, p->publish_mode,
        p->syslog_host, p->syslog_port, p->syslog_proto,
        p->syslog_tls_insecure ? "true" : "false", p->syslog_mqtt,
        p->mqtt_tls_insecure ? "true" : "false");
    httpd_resp_send_chunk(req, buf, n);

    n = snprintf(buf, 1024,
        "\"static_ip\":\"%s\",\"static_gw\":\"%s\",\"static_mask\":\"%s\","
        "\"dns_target\":\"%s\",\"ntp_target\":\"%s\","
        "\"wan_open\":%s,\"cloud_forward\":%s,"
        "\"dns_redirect\":%s,\"ntp_redirect\":%s,"
        "\"allow_dns_offline\":%s,\"allow_ntp_offline\":%s,",
        p->static_ip, p->static_gw, p->static_mask,
        p->dns_target, p->ntp_target,
        p->wan_open ? "true" : "false",
        p->cloud_forward ? "true" : "false",
        p->dns_redirect ? "true" : "false",
        p->ntp_redirect ? "true" : "false",
        p->allow_dns_offline ? "true" : "false",
        p->allow_ntp_offline ? "true" : "false");
    httpd_resp_send_chunk(req, buf, n);

    n = snprintf(buf, 1024,
        "\"ntp_server\":\"%s\",\"tz\":\"%s\",\"dst_mode\":%d,"
        "\"ota_url\":\"%s\",\"ota_check\":%s,\"ota_auto\":%s,"
        "\"admin_pass\":\"%s\",",
        p->ntp_server, p->tz, p->dst_mode,
        p->ota_url,
        p->ota_check ? "true" : "false",
        p->ota_auto ? "true" : "false",
        p->admin_pass);
    httpd_resp_send_chunk(req, buf, n);
    free(p);

    // Device names, so a restore keeps the Home Assistant entity ids.
    httpd_resp_send_chunk(req, "\"devices\":[", HTTPD_RESP_USE_STRLEN);
    int listed = 0;
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        device_registry_lock();
        device_entry_t *d = device_registry_at(i);
        char mac[DEV_MAC_LEN] = "", slug[DEV_SLUG_LEN] = "",
             name[DEV_NAME_LEN] = "";
        if (d) {
            strncpy(mac, d->mac, sizeof(mac) - 1);
            strncpy(slug, d->slug, sizeof(slug) - 1);
            strncpy(name, d->name, sizeof(name) - 1);
        }
        device_registry_unlock();
        if (!mac[0]) continue;

        n = snprintf(buf, 1024,
                     "%s{\"mac\":\"%s\",\"slug\":\"%s\",\"name\":\"%s\"}",
                     listed ? "," : "", mac, slug, name);
        httpd_resp_send_chunk(req, buf, n);
        listed++;
    }
    httpd_resp_send_chunk(req, "],", HTTPD_RESP_USE_STRLEN);

    // The CA certificate last: it is the largest field and needs its
    // newlines escaped.
    char *pem = malloc(2048);
    if (pem && sb_prov_ca_cert_load(pem, 2048) > 0) {
        httpd_resp_send_chunk(req, "\"mqtt_ca\":\"", HTTPD_RESP_USE_STRLEN);
        for (const char *c = pem; *c; c++) {
            const char *esc = (*c == '\n') ? "\\n"
                            : (*c == '\r') ? ""
                            : (*c == '"')  ? "\\\""
                            : (*c == '\\') ? "\\\\" : NULL;
            if (esc) {
                if (*esc) httpd_resp_send_chunk(req, esc, strlen(esc));
            } else {
                httpd_resp_send_chunk(req, c, 1);
            }
        }
        httpd_resp_send_chunk(req, "\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req, "\"mqtt_ca\":\"\"}", HTTPD_RESP_USE_STRLEN);
    }
    free(pem);
    free(buf);

    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "Configuration backup downloaded");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Restore
// ---------------------------------------------------------------------------

static void json_str(cJSON *root, const char *key, char *dst, size_t dst_sz)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, dst_sz - 1);
        dst[dst_sz - 1] = '\0';
    }
}

static void json_bool(cJSON *root, const char *key, bool *dst)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (cJSON_IsBool(v)) *dst = cJSON_IsTrue(v);
}

static void json_int(cJSON *root, const char *key, int *dst)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(v)) *dst = (int)v->valuedouble;
}

esp_err_t restore_post_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    if (req->content_len <= 0 || req->content_len > 8192) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "File missing or too large");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Not enough memory right now");
        return ESP_OK;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "Upload failed");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "That file is not valid JSON");
        return ESP_OK;
    }

    // Refuse an unknown format rather than importing fields that may
    // have changed meaning.
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsNumber(ver) || (int)ver->valuedouble != BACKUP_VERSION) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Unsupported backup version");
        return ESP_OK;
    }

    sb_prov_cfg_t *p = malloc(sizeof(*p));
    if (!p) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Not enough memory right now");
        return ESP_OK;
    }
    // Start from current values so a field absent from the backup keeps
    // whatever is configured now.
    sb_prov_load(p);

    json_str(root, "sta_ssid", p->sta_ssid, sizeof(p->sta_ssid));
    json_str(root, "sta_pass", p->sta_pass, sizeof(p->sta_pass));
    json_str(root, "ap_ssid", p->ap_ssid, sizeof(p->ap_ssid));
    json_str(root, "ap_pass", p->ap_pass, sizeof(p->ap_pass));
    json_str(root, "ap_ip", p->ap_ip, sizeof(p->ap_ip));
    {
        int px = p->ap_prefix;
        json_int(root, "ap_prefix", &px);
        if (px >= 8 && px <= 30) p->ap_prefix = px;
    }
    json_bool(root, "ap_hidden", &p->ap_hidden);
    json_int(root, "ap_channel", &p->ap_channel);

    json_str(root, "mqtt_uri", p->ha_mqtt_uri, sizeof(p->ha_mqtt_uri));
    json_str(root, "mqtt_user", p->ha_mqtt_user, sizeof(p->ha_mqtt_user));
    json_str(root, "mqtt_pass", p->ha_mqtt_pass, sizeof(p->ha_mqtt_pass));
    json_str(root, "device_id", p->device_id, sizeof(p->device_id));
    json_str(root, "bridge_name", p->bridge_name, sizeof(p->bridge_name));
    json_str(root, "syslog_host", p->syslog_host, sizeof(p->syslog_host));
    {
        cJSON *sp = cJSON_GetObjectItem(root, "syslog_port");
        if (cJSON_IsNumber(sp)) p->syslog_port = sp->valueint;
        cJSON *pr = cJSON_GetObjectItem(root, "syslog_proto");
        if (cJSON_IsNumber(pr) && pr->valueint >= 0 && pr->valueint <= 2)
            p->syslog_proto = pr->valueint;
        cJSON *si = cJSON_GetObjectItem(root, "syslog_tls_insecure");
        if (cJSON_IsBool(si)) p->syslog_tls_insecure = cJSON_IsTrue(si);
        cJSON *sm = cJSON_GetObjectItem(root, "syslog_mqtt");
        if (cJSON_IsNumber(sm) && sm->valueint >= 0 && sm->valueint <= 2)
            p->syslog_mqtt = sm->valueint;
    }
    json_int(root, "publish_mode", &p->publish_mode);
    json_bool(root, "mqtt_tls_insecure", &p->mqtt_tls_insecure);

    json_str(root, "static_ip", p->static_ip, sizeof(p->static_ip));
    json_str(root, "static_gw", p->static_gw, sizeof(p->static_gw));
    json_str(root, "static_mask", p->static_mask, sizeof(p->static_mask));
    json_str(root, "dns_target", p->dns_target, sizeof(p->dns_target));
    json_str(root, "ntp_target", p->ntp_target, sizeof(p->ntp_target));

    json_bool(root, "wan_open", &p->wan_open);
    json_bool(root, "cloud_forward", &p->cloud_forward);
    json_bool(root, "dns_redirect", &p->dns_redirect);
    json_bool(root, "ntp_redirect", &p->ntp_redirect);
    json_bool(root, "allow_dns_offline", &p->allow_dns_offline);
    json_bool(root, "allow_ntp_offline", &p->allow_ntp_offline);

    json_str(root, "ntp_server", p->ntp_server, sizeof(p->ntp_server));
    json_str(root, "tz", p->tz, sizeof(p->tz));
    json_int(root, "dst_mode", &p->dst_mode);
    json_str(root, "ota_url", p->ota_url, sizeof(p->ota_url));
    json_bool(root, "ota_check", &p->ota_check);
    json_bool(root, "ota_auto", &p->ota_auto);
    json_str(root, "admin_pass", p->admin_pass, sizeof(p->admin_pass));

    sb_prov_save(p);
    free(p);

    cJSON *ca = cJSON_GetObjectItem(root, "mqtt_ca");
    if (cJSON_IsString(ca) && ca->valuestring && ca->valuestring[0]) {
        sb_prov_ca_cert_save(ca->valuestring);
    }

    // Device names, so Home Assistant entity ids survive the restore.
    cJSON *devs = cJSON_GetObjectItem(root, "devices");
    int restored = 0;
    if (cJSON_IsArray(devs)) {
        cJSON *d = NULL;
        cJSON_ArrayForEach(d, devs) {
            cJSON *mac = cJSON_GetObjectItem(d, "mac");
            cJSON *slug = cJSON_GetObjectItem(d, "slug");
            cJSON *name = cJSON_GetObjectItem(d, "name");
            if (!cJSON_IsString(mac)) continue;

            // Create the entry, then apply the stored names.
            if (device_registry_resolve(mac->valuestring)) {
                device_registry_rename(
                    mac->valuestring,
                    cJSON_IsString(slug) ? slug->valuestring : NULL,
                    cJSON_IsString(name) ? name->valuestring : NULL);
                restored++;
            }
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Configuration restored (%d device name%s) — restarting",
             restored, restored == 1 ? "" : "s");

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "Restored. Restarting.");

    sb_system_reboot(1500);
    return ESP_OK;
}
