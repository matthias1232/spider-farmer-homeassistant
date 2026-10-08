#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "provisioning.h"
#include "device_registry.h"
#include "wifi_apsta.h"
#include "improv_serial.h"

static const char *TAG = "improv";

// Packet: "IMPROV" | version 1 | type | length | data | checksum (sum of
// all previous bytes) -- followed by a newline, as ESPHome sends it.
#define IMPROV_VERSION 1

enum { T_STATE = 0x01, T_ERROR = 0x02, T_RPC = 0x03, T_RPC_RESULT = 0x04 };
enum { CMD_WIFI = 0x01, CMD_STATE = 0x02, CMD_INFO = 0x03, CMD_SCAN = 0x04 };
enum { ST_READY = 0x02, ST_PROVISIONING = 0x03, ST_PROVISIONED = 0x04 };
enum { ERR_NONE = 0x00, ERR_INVALID = 0x01, ERR_UNKNOWN_CMD = 0x02,
       ERR_CONNECT = 0x03, ERR_UNKNOWN = 0xFF };

#define UART_PORT UART_NUM_0

static void send_packet(uint8_t type, const uint8_t *data, uint8_t len)
{
    uint8_t buf[9 + 255 + 2];
    memcpy(buf, "IMPROV", 6);
    buf[6] = IMPROV_VERSION;
    buf[7] = type;
    buf[8] = len;
    if (len) memcpy(buf + 9, data, len);
    uint8_t sum = 0;
    for (int i = 0; i < 9 + len; i++) sum += buf[i];
    buf[9 + len] = sum;
    buf[10 + len] = '\n';
    uart_write_bytes(UART_PORT, (const char *)buf, 11 + len);
}

static void send_state(uint8_t st)  { send_packet(T_STATE, &st, 1); }
static void send_error(uint8_t err) { send_packet(T_ERROR, &err, 1); }

// RPC result: command | total length | (len | string)...
static void send_result(uint8_t cmd, const char *const *strs, int n)
{
    uint8_t d[250];
    int p = 2;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(strs[i]);
        if (p + 1 + l > sizeof(d)) break;
        d[p++] = (uint8_t)l;
        memcpy(d + p, strs[i], l);
        p += l;
    }
    d[0] = cmd;
    d[1] = (uint8_t)(p - 2);
    send_packet(T_RPC_RESULT, d, (uint8_t)p);
}

static bool sta_url(char *out, size_t n)
{
    esp_netif_ip_info_t ip = {0};
    if (!wifi_apsta_uplink_is_up() || esp_netif_get_ip_info(g_sta_netif, &ip) != ESP_OK || !ip.ip.addr)
        return false;
    snprintf(out, n, "http://" IPSTR "/", IP2STR(&ip.ip));
    return true;
}

static void handle_rpc(const uint8_t *d, uint8_t len)
{
    if (len < 2 || d[1] != len - 2) { send_error(ERR_INVALID); return; }
    uint8_t cmd = d[0];
    const uint8_t *a = d + 2;
    uint8_t alen = d[1];

    switch (cmd) {
    case CMD_STATE: {
        char url[40];
        if (sta_url(url, sizeof(url))) {
            send_state(ST_PROVISIONED);
            const char *s[] = { url };
            send_result(CMD_STATE, s, 1);
        } else {
            send_state(ST_READY);
        }
        break;
    }
    case CMD_INFO: {
        const esp_app_desc_t *app = esp_app_get_description();
        const char *s[] = { "SpiderBridge", app->version, "ESP32", prov_bridge_name() };
        send_result(CMD_INFO, s, 4);
        break;
    }
    case CMD_SCAN: {
        wifi_scan_ap_t *ap = calloc(20, sizeof(*ap));
        int n = ap ? wifi_apsta_scan(ap, 20) : 0;
        for (int i = 0; i < n; i++) {
            char rssi[8];
            snprintf(rssi, sizeof(rssi), "%d", ap[i].rssi);
            const char *s[] = { ap[i].ssid, rssi, ap[i].auth ? "YES" : "NO" };
            send_result(CMD_SCAN, s, 3);
        }
        free(ap);
        send_result(CMD_SCAN, NULL, 0);          // end of list
        break;
    }
    case CMD_WIFI: {
        if (alen < 1 || a[0] + 1 > alen) { send_error(ERR_INVALID); return; }
        uint8_t sl = a[0];
        if (sl + 2 > alen || sl + 2 + a[sl + 1] > alen || sl > 32 || a[sl + 1] > 64) {
            send_error(ERR_INVALID); return;
        }
        char ssid[33], pass[65];
        memcpy(ssid, a + 1, sl); ssid[sl] = '\0';
        uint8_t pl = a[sl + 1];
        memcpy(pass, a + sl + 2, pl); pass[pl] = '\0';

        ESP_LOGI(TAG, "Wi-Fi settings received over serial (installer): \"%s\"", ssid);
        send_state(ST_PROVISIONING);
        char msg[160];
        sb_prov_cfg_t *p = malloc(sizeof(*p));
        if (!p) { send_error(ERR_UNKNOWN); return; }
        sb_prov_load(p);
        bool was_configured = p->sta_ssid[0] != '\0';
        if (!wifi_apsta_connect_now(ssid, pass, 20000, msg, sizeof(msg))) {
            ESP_LOGW(TAG, "%s", msg);
            send_error(ERR_CONNECT);
            send_state(ST_READY);
            free(p);
            return;
        }
        strncpy(p->sta_ssid, ssid, sizeof(p->sta_ssid) - 1);
        p->sta_ssid[sizeof(p->sta_ssid) - 1] = '\0';
        strncpy(p->sta_pass, pass, sizeof(p->sta_pass) - 1);
        p->sta_pass[sizeof(p->sta_pass) - 1] = '\0';
        sb_prov_save(p);
        memset(pass, 0, sizeof(pass));
        memset(p, 0, sizeof(*p));
        free(p);
        ESP_LOGI(TAG, "%s -- stored", msg);

        char url[40] = "";
        sta_url(url, sizeof(url));
        send_error(ERR_NONE);
        send_state(ST_PROVISIONED);
        const char *s[] = { url };
        send_result(CMD_WIFI, s, url[0] ? 1 : 0);
        // An unconfigured bridge started only the hotspot and the settings
        // page; a restart brings up everything that needs the home network.
        if (!was_configured) {
            ESP_LOGI(TAG, "First home network set -- restarting to start all services");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }
        break;
    }
    default:
        send_error(ERR_UNKNOWN_CMD);
    }
}

// Memory matters: the task and the UART driver cost ~5 KB, which the TLS
// sessions need. So the listener runs only while it can be useful -- the
// installer talks to the device right after flashing (unconfigured) or
// right after a reset/plug-in -- and then frees everything.
#define IMPROV_WINDOW_MS (5 * 60 * 1000)

static void improv_task(void *arg)
{
    bool forever = (bool)(intptr_t)arg;
    uint8_t buf[9 + 255 + 1];
    int n = 0;
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(IMPROV_WINDOW_MS);
    for (;;) {
        uint8_t c;
        if (!forever && (int32_t)(xTaskGetTickCount() - until) >= 0) break;
        if (uart_read_bytes(UART_PORT, &c, 1, pdMS_TO_TICKS(1000)) != 1) continue;
        // Sync on "IMPROV": anything else on the line is ignored.
        if (n < 6) {
            if (c == (uint8_t)"IMPROV"[n]) buf[n++] = c;
            else n = (c == 'I') ? (buf[0] = c, 1) : 0;
            continue;
        }
        buf[n++] = c;
        if (n < 9) continue;
        uint8_t len = buf[8];
        if (n < 9 + len + 1) continue;
        uint8_t sum = 0;
        for (int i = 0; i < 9 + len; i++) sum += buf[i];
        uint8_t type = buf[7], ver = buf[6];
        bool ok = sum == buf[9 + len] && ver == IMPROV_VERSION;
        n = 0;
        if (!ok) { send_error(ERR_INVALID); continue; }
        if (type == T_RPC) handle_rpc(buf + 9, len);
    }
    uart_driver_delete(UART_PORT);
    ESP_LOGI(TAG, "Serial Wi-Fi setup (Improv) closed after %d min -- memory released",
             IMPROV_WINDOW_MS / 60000);
    vTaskDelete(NULL);
}

void improv_serial_start(void)
{
    sb_prov_cfg_t *p = malloc(sizeof(*p));
    bool configured = false;
    if (p) { sb_prov_load(p); configured = p->sta_ssid[0] != '\0'; free(p); }

    // RX only through the driver; log output keeps using the console path.
    if (!uart_is_driver_installed(UART_PORT) &&
        uart_driver_install(UART_PORT, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "Serial Wi-Fi setup (Improv) not available");
        return;
    }
    // A configured bridge needs the listener only right after flashing or
    // a plug-in: 5 minutes. But its ~5 KB are the margin the controller's
    // first TLS handshake needs, so on a configured bridge it is skipped
    // when a controller is already known -- the installer is used before
    // any controller exists.
    if (configured && device_registry_count() > 0) {
        uart_driver_delete(UART_PORT);
        ESP_LOGI(TAG, "Serial Wi-Fi setup (Improv) off: bridge configured and controllers known");
        return;
    }
    if (xTaskCreate(improv_task, "improv", 3584, (void *)(intptr_t)!configured, 3, NULL) != pdPASS) {
        uart_driver_delete(UART_PORT);
        return;
    }
    if (configured)
        ESP_LOGI(TAG, "Serial Wi-Fi setup (Improv) ready on the USB port for %d minutes",
                 IMPROV_WINDOW_MS / 60000);
    else
        ESP_LOGI(TAG, "Serial Wi-Fi setup (Improv) ready on the USB port (no home network yet)");
}
