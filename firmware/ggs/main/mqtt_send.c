#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "sb_config.h"
#include "device_registry.h"
#include "mitm_proxy.h"
#include "sf_command_handler.h"
#include "ha_mqtt.h"
#include "web_auth.h"
#include "mqtt_send.h"

static const char *TAG = "mqtt_send";

static void url_decode_inplace(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) &&
                                isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static bool form_field(const char *body, const char *key, char *out, size_t out_sz)
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
            url_decode_inplace(out);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

// Message ids follow the controller's own format, so a hand-sent
// request is indistinguishable from a normal one and gets answered the
// same way.
static void make_msg_id(char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%08lx%04x",
             (unsigned long)(esp_timer_get_time() / 1000),
             (unsigned)(esp_random() & 0xFFFF));
}

static void reply(httpd_req_t *req, int status, const char *text)
{
    if (status != 200) {
        httpd_resp_set_status(req, status == 409 ? "409 Conflict"
                                                 : "400 Bad Request");
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

esp_err_t mqtt_send_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    if (req->content_len <= 0 || req->content_len > 2048) {
        reply(req, 400, "Request body missing or too large");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        reply(req, 400, "Out of memory");
        return ESP_OK;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            free(body);
            reply(req, 400, "Receive failed");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = '\0';

    char mode[16] = "", mac[DEV_MAC_LEN] = "";
    char *payload = malloc(1600);
    char *method = malloc(64);
    if (!payload || !method) {
        free(body); free(payload); free(method);
        reply(req, 400, "Out of memory");
        return ESP_OK;
    }
    payload[0] = '\0';
    method[0] = '\0';

    form_field(body, "mode", mode, sizeof(mode));
    form_field(body, "d", mac, sizeof(mac));
    form_field(body, "method", method, 64);
    form_field(body, "p", payload, 1600);
    free(body);

    // Default to the first connected controller so the common case
    // needs no device picked.
    if (!mac[0]) {
        const char *m = mitm_proxy_controller_mac();
        if (m) strncpy(mac, m, sizeof(mac) - 1);
    }
    if (!mac[0] || !mitm_proxy_device_online(mac)) {
        free(payload); free(method);
        reply(req, 409, "No controller connected");
        return ESP_OK;
    }

    const char *uid = mitm_proxy_uid_for(mac);

    // --- raw: sent exactly as given ---
    if (strcmp(mode, "raw") == 0) {
        if (!payload[0]) {
            free(payload); free(method);
            reply(req, 400, "Nothing to send");
            return ESP_OK;
        }
        // Parsed only to catch malformed JSON early; the original text
        // is what gets sent, so nothing is reformatted or reordered.
        cJSON *check = cJSON_Parse(payload);
        if (!check) {
            free(payload); free(method);
            reply(req, 400, "That is not valid JSON");
            return ESP_OK;
        }
        cJSON_Delete(check);

        mitm_proxy_inject_command_to(mac, payload, strlen(payload));
        ESP_LOGI(TAG, "Raw command sent to %s (%u bytes)", mac,
                 (unsigned)strlen(payload));
        free(payload); free(method);
        reply(req, 200, "Sent — watch the log for the reply");
        return ESP_OK;
    }

    // --- method: envelope filled in automatically ---
    if (strcmp(mode, "method") == 0) {
        if (!method[0]) {
            free(payload); free(method);
            reply(req, 400, "Method name missing");
            return ESP_OK;
        }

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "method", method);
        cJSON_AddStringToObject(root, "pid", mac);

        if (payload[0]) {
            cJSON *params = cJSON_Parse(payload);
            if (!params) {
                cJSON_Delete(root);
                free(payload); free(method);
                reply(req, 400, "Params are not valid JSON");
                return ESP_OK;
            }
            cJSON_AddItemToObject(root, "params", params);
        }

        char msg_id[32];
        make_msg_id(msg_id, sizeof(msg_id));
        cJSON_AddStringToObject(root, "msgId", msg_id);
        cJSON_AddStringToObject(root, "uid", uid ? uid : "");

        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json) {
            free(payload); free(method);
            reply(req, 400, "Could not build the message");
            return ESP_OK;
        }

        mitm_proxy_inject_command_to(mac, json, strlen(json));
        ESP_LOGI(TAG, "Sent '%s' to %s", method, mac);
        cJSON_free(json);

        free(payload); free(method);
        reply(req, 200, "Sent — watch the log for the reply");
        return ESP_OK;
    }

    // --- ha: through the Home Assistant translation ---
    //
    // "method" carries the field (and optionally field/subfield), and
    // the payload is the value — the same three pieces an MQTT command
    // topic encodes. Going through sf_translate_command means this path
    // produces byte-identical output to a real Home Assistant command,
    // which is the point: it tests the actual path, not an imitation.
    if (strcmp(mode, "ha") == 0) {
        if (!method[0]) {
            free(payload); free(method);
            reply(req, 400, "Field name missing");
            return ESP_OK;
        }

        char field[32] = "", sub[32] = "";
        char *slash = strchr(method, '/');
        if (slash) {
            *slash = '\0';
            strncpy(field, method, sizeof(field) - 1);
            strncpy(sub, slash + 1, sizeof(sub) - 1);
        } else {
            strncpy(field, method, sizeof(field) - 1);
        }

        cJSON *cmd = sf_translate_command(field, sub[0] ? sub : NULL,
                                          payload, mac, uid ? uid : "");
        if (!cmd) {
            free(payload); free(method);
            reply(req, 400, "Could not translate that field and value");
            return ESP_OK;
        }

        char *json = cJSON_PrintUnformatted(cmd);
        cJSON_Delete(cmd);
        if (!json) {
            free(payload); free(method);
            reply(req, 400, "Could not build the message");
            return ESP_OK;
        }

        mitm_proxy_inject_command_to(mac, json, strlen(json));
        ESP_LOGI(TAG, "Sent %s/%s = %s to %s", field, sub[0] ? sub : "-",
                 payload, mac);
        cJSON_free(json);

        free(payload); free(method);
        reply(req, 200, "Sent — watch the log for the reply");
        return ESP_OK;
    }

    free(payload); free(method);
    reply(req, 400, "Unknown mode");
    return ESP_OK;
}
