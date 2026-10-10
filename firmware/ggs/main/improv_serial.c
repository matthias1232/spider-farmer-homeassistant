#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

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
#include "ggs_ble.h"
#include "supervisor.h"
#include "fault_inject.h"

static const char *TAG = "improv";

// Packet: "IMPROV" | version 1 | type | length | data | checksum (sum of
// all previous bytes) -- followed by a newline, as ESPHome sends it.
#define IMPROV_VERSION 1

enum { T_STATE = 0x01, T_ERROR = 0x02, T_RPC = 0x03, T_RPC_RESULT = 0x04 };
enum { CMD_WIFI = 0x01, CMD_STATE = 0x02, CMD_INFO = 0x03, CMD_SCAN = 0x04,
       // SpiderBridge extensions. The Improv spec reserves 0x01..0x04; other
       // Improv clients never send these and get UNKNOWN_RPC_COMMAND for them.
       CMD_X_NETINFO = 0x40,   // -> addresses and status, see handle_netinfo()
       CMD_X_AP_PASS = 0x41,   // [pass] ("" = random) -> hotspot password
       CMD_X_WIFI_AP = 0x42,   // [ssid][pass][ap pass][quick] join + hotspot password
                               // optional last string "1" = quick connect, see below
       CMD_X_BLE     = 0x43, // -> Bluetooth side: armed flag, last result, controllers found
       CMD_X_RESTART = 0x44, // restart the bridge (answers first)
       CMD_X_DIAG    = 0x45, // -> why it last restarted, restart counters, heap, uptime
       CMD_X_FAULT   = 0x46, // ["CRASH-TEST"]["panic|taskwdt|intwdt|deadlock|leak"] self-test, see fault_inject.h
       // Spider Farmer GGS controllers over Bluetooth. Both start a Bluetooth-only boot (the board is silent on
       // USB for 10-120 s), so the caller polls CMD_X_BLE_JOB until the board answers again.
       CMD_X_BLE_SCAN = 0x47, // search for controllers again -> ["1"] started, ["0"] busy
       CMD_X_BLE_SEND = 0x48, // [address] give this controller the bridge's hotspot Wi-Fi -> ["1"] / ["0"] busy
       CMD_X_BLE_JOB  = 0x49 }; // -> outcome of the last send: [state, address, text, pending]
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

// Reads one length-prefixed string from the argument block. Fails when it
// does not fit the block or the destination (cap includes the terminator).
static bool take_str(const uint8_t *a, uint8_t alen, uint8_t *pos, char *out, size_t cap)
{
    if (*pos >= alen) return false;
    uint8_t l = a[*pos];
    if ((unsigned)*pos + 1u + l > alen || (size_t)l >= cap) return false;
    memcpy(out, a + *pos + 1, l);
    out[l] = '\0';
    *pos = (uint8_t)(*pos + 1 + l);
    return true;
}

// Everything the installer shows under "IP addresses". Strings, in order:
//   0 uplink SSID      ("" when none)       5 hotspot MAC
//   1 uplink IP        ("" when no address) 6 hotspot clients (decimal)
//   2 uplink gateway                        7 uplink RSSI in dBm (decimal)
//   3 hotspot SSID                          8 bridge name
//   4 hotspot IP                            9 firmware version
static void handle_netinfo(void)
{
    // On the heap: this task's stack is only 3.5 KB.
    wifi_status_t *w = calloc(1, sizeof(*w));
    if (!w) { send_error(ERR_UNKNOWN); return; }
    wifi_apsta_get_status(w);
    char clients[8], rssi[8];
    snprintf(clients, sizeof(clients), "%d", w->ap_clients);
    snprintf(rssi, sizeof(rssi), "%d", w->sta_rssi);
    const esp_app_desc_t *app = esp_app_get_description();
    const char *s[] = { w->sta_ssid, w->sta_ip, w->sta_gateway,
                        w->ap_ssid, w->ap_ip, w->ap_mac, clients, rssi,
                        prov_bridge_name(), app->version };
    send_result(CMD_X_NETINFO, s, 10);
    free(w);
}

// The hotspot restarts with the new password; a controller on it has to be
// told the new one (app or Bluetooth), so a restart right here is the
// honest way to make the change real instead of leaving it half-applied.
static void restart_soon(void)
{
    ESP_LOGW(TAG, "Hotspot password changed -- restarting to apply it");
    vTaskDelay(pdMS_TO_TICKS(2500));
    sv_restart(SV_WHY_USER);
}

static void handle_ap_pass(const uint8_t *a, uint8_t alen)
{
    char pw[65];
    uint8_t pos = 0;
    if (!take_str(a, alen, &pos, pw, sizeof(pw)) || pos != alen) { send_error(ERR_INVALID); return; }
    if (pw[0] == '\0') sb_prov_random_ap_pass(pw, 16);
    if (!sb_prov_store_ap_pass(pw)) { memset(pw, 0, sizeof(pw)); send_error(ERR_INVALID); return; }

    wifi_status_t *w = calloc(1, sizeof(*w));
    if (w) wifi_apsta_get_status(w);
    const char *s[] = { w ? w->ap_ssid : "", pw };
    send_result(CMD_X_AP_PASS, s, 2);
    memset(pw, 0, sizeof(pw));
    free(w);
    restart_soon();
}

// Join the home network and set a new hotspot password in one step. Nothing
// is stored unless the network was joined, so a typo in the Wi-Fi password
// cannot leave the bridge with a new hotspot password and no uplink.
static void handle_wifi_ap(const uint8_t *a, uint8_t alen)
{
    char ssid[33], pass[65], ap_pass[65], quick[4] = "";
    uint8_t pos = 0;
    if (!take_str(a, alen, &pos, ssid, sizeof(ssid)) ||
        !take_str(a, alen, &pos, pass, sizeof(pass)) ||
        !take_str(a, alen, &pos, ap_pass, sizeof(ap_pass)) || !ssid[0]) {
        send_error(ERR_INVALID); return;
    }
    // Optional fourth string: "1" asks for quick connect (see below).
    if (pos < alen && !take_str(a, alen, &pos, quick, sizeof(quick))) { send_error(ERR_INVALID); return; }
    if (pos != alen) { send_error(ERR_INVALID); return; }
    if (ap_pass[0] == '\0') sb_prov_random_ap_pass(ap_pass, 16);
    if (strlen(ap_pass) < 8 || strlen(ap_pass) > 63) { send_error(ERR_INVALID); return; }

    ESP_LOGI(TAG, "Wi-Fi + hotspot password received over serial (installer): \"%s\"", ssid);
    send_state(ST_PROVISIONING);
    sb_prov_cfg_t *p = malloc(sizeof(*p));
    if (!p) { send_error(ERR_UNKNOWN); return; }
    sb_prov_load(p);
    char msg[160];
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
    strncpy(p->ap_pass, ap_pass, sizeof(p->ap_pass) - 1);
    p->ap_pass[sizeof(p->ap_pass) - 1] = '\0';
    sb_prov_save(p);
    // Quick connect: on the next start the Bluetooth scan also puts the GGS controllers
    // it finds onto this hotspot. Set only now, after the network was really joined, so a
    // failed attempt cannot leave it armed.
    sb_prov_set_auto_ble(quick[0] == '1');

    char url[40] = "";
    sta_url(url, sizeof(url));
    send_error(ERR_NONE);
    send_state(ST_PROVISIONED);
    const char *s[] = { url, p->ap_ssid, ap_pass };
    send_result(CMD_X_WIFI_AP, s, 3);
    memset(pass, 0, sizeof(pass));
    memset(ap_pass, 0, sizeof(ap_pass));
    memset(p, 0, sizeof(*p));
    free(p);
    restart_soon();
}

// Strings: 0 "1" when quick connect is armed for the next start, else "0";
//          1 last Bluetooth result ("" when none); 2 number of controllers
//          found by the last scan; 3.. one "address|name|rssi|flags" per controller (flags: bit 0 bound to an
//          account, bit 1 on Wi-Fi, bit 3 cloud session; -1 when not advertised).
// Read only; it does not start a scan (a scan restarts the bridge into Bluetooth).
static void handle_ble(void)
{
    ggs_ble_status_t *b = calloc(1, sizeof(*b));
    if (!b) { send_error(ERR_UNKNOWN); return; }
    ggs_ble_get_status(b);
    char n[8];
    snprintf(n, sizeof(n), "%d", b->scanned ? b->count : 0);
    char rows[GGS_BLE_MAX_FOUND][64];
    const char *s[3 + GGS_BLE_MAX_FOUND];
    s[0] = sb_prov_auto_ble() ? "1" : "0";
    s[1] = b->last_result;
    s[2] = n;
    int k = 3;
    for (int i = 0; b->scanned && i < b->count && i < GGS_BLE_MAX_FOUND; i++) {
        snprintf(rows[i], sizeof(rows[i]), "%s|%s|%d|%d", b->dev[i].addr, b->dev[i].name, (int)b->dev[i].rssi,
                 b->dev[i].has_flags ? (int)b->dev[i].flags : -1);
        s[k++] = rows[i];
    }
    send_result(CMD_X_BLE, s, k);
    free(b);
}

// Restart on request. Answers first, so the installer knows it was accepted, then restarts.
static void handle_restart(void)
{
    const char *s[] = { "1" };
    send_result(CMD_X_RESTART, s, 1);
    ESP_LOGW(TAG, "Restart requested over USB");
    vTaskDelay(pdMS_TO_TICKS(400));
    sv_restart(SV_WHY_USER);
}

// What an owner needs to know about a bridge that "restarted by itself", and what the installer shows as
// diagnostics. Strings, in order:
//   0 reset reason of this start ("Power on", "Task watchdog", ...)   6 task the supervisor restarted
//   1 starts since the last factory reset                                     for, "" when none
//   2 restarts in a row without a healthy run (3 = safe mode)         7 uptime in seconds
//   3 crashes and watchdog resets in total                            8 free heap in bytes
//   4 "1" when running in SAFE MODE, else "0"                         9 lowest free heap since start
//   5 why the supervisor itself last restarted (number, 0 = never)   10 largest free block
static void handle_diag(void)
{
    sv_info_t *i = calloc(1, sizeof(*i));
    if (!i) { send_error(ERR_UNKNOWN); return; }
    sv_get_info(i);
    char boots[12], streak[12], crashes[12], why[12], up[12], hf[12], hm[12], hl[12];
    snprintf(boots, sizeof(boots), "%u", (unsigned)i->boots);
    snprintf(streak, sizeof(streak), "%u", (unsigned)i->crash_streak);
    snprintf(crashes, sizeof(crashes), "%u", (unsigned)i->crashes);
    snprintf(why, sizeof(why), "%d", (int)i->last_sv_why);
    snprintf(up, sizeof(up), "%u", (unsigned)i->uptime_s);
    snprintf(hf, sizeof(hf), "%u", (unsigned)i->heap_free);
    snprintf(hm, sizeof(hm), "%u", (unsigned)i->heap_min);
    snprintf(hl, sizeof(hl), "%u", (unsigned)i->heap_largest);
    const char *s[] = { i->reset_text, boots, streak, crashes, i->safe_mode ? "1" : "0", why,
                        i->last_sv_task, up, hf, hm, hl };
    send_result(CMD_X_DIAG, s, 11);
    free(i);
}

// Search again for GGS controllers. The bridge restarts into a Bluetooth-only boot, scans, and comes back.
static void handle_ble_scan(void)
{
    const char *s[] = { ggs_ble_request_scan(1500) ? "1" : "0" };
    send_result(CMD_X_BLE_SCAN, s, 1);
}

// Give one controller the bridge's hotspot Wi-Fi (name and password as stored), over Bluetooth. Only a controller
// the last scan found is accepted -- nothing else ever receives the hotspot's credentials. The controller is left
// visible over Bluetooth (no binding), so the phone and the Spider Farmer app can still find it.
static void handle_ble_send(const uint8_t *a, uint8_t alen)
{
    char addr[24];
    uint8_t pos = 0;
    if (!take_str(a, alen, &pos, addr, sizeof(addr)) || pos != alen || strlen(addr) != 17) {
        send_error(ERR_INVALID);
        return;
    }
    ggs_ble_status_t *b = calloc(1, sizeof(*b));
    if (!b) { send_error(ERR_UNKNOWN); return; }
    ggs_ble_get_status(b);
    bool known = false;
    for (int i = 0; b->scanned && i < b->count && i < GGS_BLE_MAX_FOUND; i++) {
        if (strcasecmp(b->dev[i].addr, addr) == 0) {
            memcpy(addr, b->dev[i].addr, 18);       // the spelling the Bluetooth code stored
            known = true;
            break;
        }
    }
    free(b);
    if (!known) { send_error(ERR_INVALID); return; }
    ESP_LOGI(TAG, "Hotspot Wi-Fi for controller %s requested over USB (installer)", addr);
    const char *s[] = { ggs_ble_request_provision(addr, false, 1500) ? "1" : "0" };
    send_result(CMD_X_BLE_SEND, s, 1);
}

// Outcome of the last send. Strings: 0 state ("0" none, "1" pending, "2" accepted, "3" failed), 1 controller
// address, 2 the text a person would read, 3 "1" while a Bluetooth boot is still requested but has not begun.
static void handle_ble_job(void)
{
    ggs_ble_status_t *b = calloc(1, sizeof(*b));
    if (!b) { send_error(ERR_UNKNOWN); return; }
    ggs_ble_get_status(b);
    char job[4];
    snprintf(job, sizeof(job), "%u", (unsigned)b->job);
    const char *s[] = { job, b->job_addr, b->last_result, b->pending ? "1" : "0" };
    send_result(CMD_X_BLE_JOB, s, 4);
    free(b);
}

// Self-test of the recovery layers: makes the bridge fail on purpose. Only over the USB cable, and only with
// the confirmation word, so it cannot happen by accident. See fault_inject.h.
static void handle_fault(const uint8_t *a, uint8_t alen)
{
    char word[16], kind[16];
    uint8_t pos = 0;
    if (!take_str(a, alen, &pos, word, sizeof(word)) || !take_str(a, alen, &pos, kind, sizeof(kind)) ||
        pos != alen || strcmp(word, "CRASH-TEST") != 0 || !fault_known(kind)) {
        send_error(ERR_INVALID);
        return;
    }
    const char *s[] = { kind };
    send_result(CMD_X_FAULT, s, 1);
    fault_run(kind);
}

static void handle_rpc(const uint8_t *d, uint8_t len)
{
    if (len < 2 || d[1] != len - 2) { send_error(ERR_INVALID); return; }
    uint8_t cmd = d[0];
    const uint8_t *a = d + 2;
    uint8_t alen = d[1];

    switch (cmd) {
    case CMD_X_NETINFO: handle_netinfo(); break;
    case CMD_X_BLE:     handle_ble(); break;
    case CMD_X_RESTART: handle_restart(); break;
    case CMD_X_DIAG:    handle_diag(); break;
    case CMD_X_FAULT:   handle_fault(a, alen); break;
    case CMD_X_BLE_SCAN: handle_ble_scan(); break;
    case CMD_X_BLE_SEND: handle_ble_send(a, alen); break;
    case CMD_X_BLE_JOB:  handle_ble_job(); break;
    case CMD_X_AP_PASS: handle_ap_pass(a, alen); break;
    case CMD_X_WIFI_AP: handle_wifi_ap(a, alen); break;
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
            sv_restart(SV_WHY_USER);
        }
        break;
    }
    default:
        send_error(ERR_UNKNOWN_CMD);
    }
}

// The listener runs for as long as the bridge runs. The USB console and the installer must always get an
// answer -- after a flash, after a restart, after a watchdog reset -- so nothing here ever switches it off.
// It costs about 4.5 KB (task stack plus the UART buffer); the idle heap of a bridge with a controller
// connected is ~80 KB against the 56 KB a new TLS session needs, so the margin is kept.
#define IMPROV_PERIOD_S 5

static void improv_task(void *arg)
{
    uint8_t buf[9 + 255 + 1];
    int n = 0;
    // A command can take a while (joining Wi-Fi: up to ~25 s), so the supervisor's patience for this
    // task is 6 x 5 s + 30 s.
    int hb = sv_register("improv", IMPROV_PERIOD_S);
    for (;;) {
        uint8_t c;
        sv_beat(hb);
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
        if (type == T_RPC) {
            handle_rpc(buf + 9, len);
            sv_beat(hb);
        }
    }
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
    if (xTaskCreate(improv_task, "improv", 3584, NULL, 3, NULL) != pdPASS) {
        uart_driver_delete(UART_PORT);
        return;
    }
    ESP_LOGI(TAG, "Serial Wi-Fi setup (Improv) ready on the USB port%s",
             configured ? "" : " (no home network yet)");
}
