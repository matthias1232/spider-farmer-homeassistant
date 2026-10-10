// Host test for main/improv_serial.c: feeds Improv packets through the real
// parser/handlers and checks the replies and the side effects.
//
//   python firmware/ggs/host_test/run.py
//
// Replies are also written to fixtures.json so the installer's JavaScript
// test can parse the exact bytes the firmware produces.
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "esp_stub.h"
#include "provisioning.h"
#include "wifi_apsta.h"
#include "device_registry.h"
#include "improv_serial.h"
#include "ggs_ble.h"
#include "supervisor.h"
#include "fault_inject.h"

// ---- fake hardware -------------------------------------------------------
static jmp_buf g_end;          // the task never returns on its own: the fake UART (no more input) or sv_restart() ends it
static uint8_t g_rx[2048]; static size_t g_rx_n, g_rx_pos;   // host -> device
static uint8_t g_tx[8192]; static size_t g_tx_n;             // device -> host
static int g_restarts;
static char g_stored_ap_pass[65];
static bool g_connect_ok = true;
static char g_joined_ssid[33];
static sb_prov_cfg_t g_cfg;
static bool g_saved;
static unsigned g_ticks;
esp_netif_t *g_sta_netif, *g_ap_netif;

TickType_t xTaskGetTickCount(void) { return (TickType_t)g_ticks++; }
void vTaskDelay(TickType_t t) { (void)t; }
void vTaskDelete(void *h) { (void)h; }
void esp_restart(void) { g_restarts++; }
static int g_sv_why = -1;
// sv_restart() never returns on the device. The stub ends the run the same way: it jumps out of the task.
void sv_restart(sv_why_t why) { g_restarts++; g_sv_why = (int)why; longjmp(g_end, 2); }
static char g_fault[16];
bool fault_known(const char *k) { return k && (!strcmp(k,"panic") || !strcmp(k,"taskwdt") || !strcmp(k,"intwdt") || !strcmp(k,"deadlock") || !strcmp(k,"leak")); }
void fault_run(const char *k) { strcpy(g_fault, k); }
static int g_slots;
int sv_register(const char *n, uint32_t p) { (void)n; (void)p; return g_slots++; }
void sv_beat(int s) { (void)s; }
static sv_info_t g_sv;
void sv_get_info(sv_info_t *o) { *o = g_sv; }
static void (*g_task)(void *); static void *g_task_arg;
int xTaskCreate(void (*f)(void *), const char *n, int s, void *a, int p, void *h)
{ (void)n; (void)s; (void)p; (void)h; g_task = f; g_task_arg = a; return pdPASS; }
bool uart_is_driver_installed(uart_port_t p) { (void)p; return true; }
esp_err_t uart_driver_install(uart_port_t p, int a, int b, int c, void *d, int e)
{ (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
esp_err_t uart_driver_delete(uart_port_t p) { (void)p; return 0; }
int uart_write_bytes(uart_port_t p, const char *b, size_t n)
{ (void)p; memcpy(g_tx + g_tx_n, b, n); g_tx_n += n; return (int)n; }
int uart_read_bytes(uart_port_t p, void *b, uint32_t n, TickType_t t)
{
    (void)p; (void)n; (void)t;
    if (g_rx_pos >= g_rx_n) longjmp(g_end, 1);                 // no more input: stop the (endless) task
    *(uint8_t *)b = g_rx[g_rx_pos++]; return 1;
}
const esp_app_desc_t *esp_app_get_description(void)
{ static const esp_app_desc_t d = { "0.0.0+test" }; return &d; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i)
{ (void)n; i->ip.addr = 0x0A01A8C0 + (0 << 0); i->ip.addr = (192) | (168 << 8) | (1 << 16) | (77u << 24); return 0; }

bool wifi_apsta_uplink_is_up(void) { return true; }
void wifi_apsta_get_status(wifi_status_t *o)
{
    memset(o, 0, sizeof(*o));
    strcpy(o->sta_ssid, "HomeNet"); strcpy(o->sta_ip, "192.168.1.77"); strcpy(o->sta_gateway, "192.168.1.1");
    strcpy(o->ap_ssid, "SpiderBridge"); strcpy(o->ap_ip, "192.168.10.1");
    strcpy(o->ap_mac, "68:09:47:60:62:f5"); o->ap_clients = 1; o->sta_rssi = -57;
}
int wifi_apsta_scan(wifi_scan_ap_t *out, int max)
{
    if (max < 2) return 0;
    strcpy(out[0].ssid, "HomeNet"); out[0].rssi = -50; out[0].auth = 3;
    strcpy(out[1].ssid, "Guest");   out[1].rssi = -70; out[1].auth = 0;
    return 2;
}
bool wifi_apsta_connect_now(const char *ssid, const char *pass, uint32_t t, char *msg, size_t n)
{
    (void)pass; (void)t;
    snprintf(msg, n, g_connect_ok ? "joined" : "wrong password");
    if (g_connect_ok) strcpy(g_joined_ssid, ssid);
    return g_connect_ok;
}
int device_registry_count(void) { return 0; }
static bool g_auto_ble;
bool sb_prov_auto_ble(void) { return g_auto_ble; }
void sb_prov_set_auto_ble(bool on) { g_auto_ble = on; }
static ggs_ble_status_t g_ble;
void ggs_ble_get_status(ggs_ble_status_t *o) { *o = g_ble; }
const char *prov_bridge_name(void) { return "SpiderBridge 6062F5"; }
void sb_prov_load(sb_prov_cfg_t *o) { *o = g_cfg; }
void sb_prov_save(const sb_prov_cfg_t *c) { g_cfg = *c; g_saved = true; }
void sb_prov_random_ap_pass(char *out, size_t n)
{ (void)n; strcpy(out, "RandomPw#2345!x"); }
bool sb_prov_store_ap_pass(const char *pw)
{ if (!pw || strlen(pw) < 8 || strlen(pw) > 63) return false; strcpy(g_stored_ap_pass, pw); return true; }

// ---- packet helpers ------------------------------------------------------
static size_t put_rpc(uint8_t *o, uint8_t cmd, const uint8_t *args, uint8_t alen)
{
    uint8_t d[260]; d[0] = cmd; d[1] = alen; memcpy(d + 2, args, alen);
    size_t p = 0; memcpy(o, "IMPROV", 6); p = 6;
    o[p++] = 1; o[p++] = 3; o[p++] = (uint8_t)(alen + 2);
    memcpy(o + p, d, alen + 2); p += alen + 2;
    uint8_t s = 0; for (size_t i = 0; i < p; i++) s += o[i];
    o[p++] = s; o[p++] = '\n'; return p;
}
static size_t strs(uint8_t *o, const char *const *v, int n)
{ size_t p = 0; for (int i = 0; i < n; i++) { size_t l = strlen(v[i]); o[p++] = (uint8_t)l; memcpy(o + p, v[i], l); p += l; } return p; }

static void run(const uint8_t *pkt, size_t n)
{
    memcpy(g_rx, pkt, n); g_rx_n = n; g_rx_pos = 0; g_tx_n = 0;
    improv_serial_start();
    assert(g_task);
    if (setjmp(g_end) == 0) g_task(g_task_arg);
}

// Last RPC_RESULT frame for cmd -> its strings. Returns count.
static int result_strings(uint8_t cmd, char out[12][200])
{
    size_t i = 0; int cnt = -1;
    while (i + 11 <= g_tx_n) {
        if (memcmp(g_tx + i, "IMPROV", 6) != 0) { i++; continue; }
        uint8_t type = g_tx[i + 7], len = g_tx[i + 8];
        if (type == 4 && g_tx[i + 9] == cmd) {
            const uint8_t *d = g_tx + i + 9; int k = 0; size_t p = 2;
            while (p < (size_t)(2 + d[1]) && k < 12) { memcpy(out[k], d + p + 1, d[p]); out[k][d[p]] = 0; p += 1 + d[p]; k++; }
            cnt = k;
        }
        i += 9 + len + 2;
    }
    return cnt;
}
static int last_error(void)
{
    size_t i = 0; int e = -1;
    while (i + 11 <= g_tx_n) {
        if (memcmp(g_tx + i, "IMPROV", 6) != 0) { i++; continue; }
        if (g_tx[i + 7] == 2) e = g_tx[i + 9];
        i += 9 + g_tx[i + 8] + 2;
    }
    return e;
}

static FILE *fx; static int fx_first = 1;
static void fixture(const char *name)
{
    fprintf(fx, "%s\"%s\":\"", fx_first ? "" : ",", name); fx_first = 0;
    for (size_t i = 0; i < g_tx_n; i++) fprintf(fx, "%02x", g_tx[i]);
    fprintf(fx, "\"");
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static int failures;

static void reset_world(bool connect_ok)
{
    memset(&g_cfg, 0, sizeof(g_cfg)); strcpy(g_cfg.sta_ssid, "OldNet"); strcpy(g_cfg.ap_ssid, "SpiderBridge"); g_saved = false; g_restarts = 0;
    g_auto_ble = false; memset(&g_ble, 0, sizeof(g_ble)); g_sv_why = -1; memset(&g_sv, 0, sizeof(g_sv)); g_fault[0] = 0;
    g_stored_ap_pass[0] = 0; g_joined_ssid[0] = 0; g_connect_ok = connect_ok; g_task = NULL; g_ticks = 0;
}

int main(void)
{
    fx = fopen("fixtures.json", "w"); fprintf(fx, "{");
    uint8_t pkt[600]; uint8_t a[300]; size_t n; char r[12][200];

    // 1) the existing Improv commands still work (no regression)
    reset_world(true);
    n = put_rpc(pkt, 0x03, NULL, 0); run(pkt, n);
    CHECK(result_strings(0x03, r) == 4); CHECK(!strcmp(r[0], "SpiderBridge")); CHECK(!strcmp(r[1], "0.0.0+test"));
    CHECK(!strcmp(r[2], "ESP32")); CHECK(!strcmp(r[3], "SpiderBridge 6062F5")); fixture("info");

    reset_world(true);
    n = put_rpc(pkt, 0x02, NULL, 0); run(pkt, n);
    CHECK(result_strings(0x02, r) == 1); CHECK(!strcmp(r[0], "http://192.168.1.77/")); fixture("state");

    reset_world(true);
    n = put_rpc(pkt, 0x04, NULL, 0); run(pkt, n);
    CHECK(result_strings(0x04, r) == 0); fixture("scan");

    // 1b) plain Improv Wi-Fi (0x01): what the built-in 'Connect to Wi-Fi' and 'Send Wi-Fi to device' use
    reset_world(true);
    { const char *v[] = { "HomeNet", "wifipass123" }; size_t l = strs(a, v, 2); n = put_rpc(pkt, 0x01, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(result_strings(0x01, r) == 1); CHECK(!strcmp(r[0], "http://192.168.1.77/")); CHECK(!strcmp(g_cfg.sta_pass, "wifipass123")); CHECK(last_error() == 0); fixture("wifi");
    reset_world(false);
    { const char *v[] = { "HomeNet", "bad" }; size_t l = strs(a, v, 2); n = put_rpc(pkt, 0x01, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(last_error() == 3); fixture("wifi_fail");

    // 2) NETINFO
    reset_world(true);
    n = put_rpc(pkt, 0x40, NULL, 0); run(pkt, n);
    CHECK(result_strings(0x40, r) == 10);
    CHECK(!strcmp(r[0], "HomeNet")); CHECK(!strcmp(r[1], "192.168.1.77")); CHECK(!strcmp(r[2], "192.168.1.1"));
    CHECK(!strcmp(r[3], "SpiderBridge")); CHECK(!strcmp(r[4], "192.168.10.1")); CHECK(!strcmp(r[5], "68:09:47:60:62:f5"));
    CHECK(!strcmp(r[6], "1")); CHECK(!strcmp(r[7], "-57")); CHECK(!strcmp(r[8], "SpiderBridge 6062F5")); CHECK(!strcmp(r[9], "0.0.0+test"));
    CHECK(g_restarts == 0); fixture("netinfo");

    // 3) AP password: explicit
    reset_world(true);
    { const char *v[] = { "MyNewHotspot42" }; size_t l = strs(a, v, 1); n = put_rpc(pkt, 0x41, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(result_strings(0x41, r) == 2); CHECK(!strcmp(r[0], "SpiderBridge")); CHECK(!strcmp(r[1], "MyNewHotspot42"));
    CHECK(!strcmp(g_stored_ap_pass, "MyNewHotspot42")); CHECK(g_restarts == 1); CHECK(last_error() != 1); fixture("ap_pass");

    // 4) AP password: empty means random, and the random one is what is stored AND returned
    reset_world(true);
    { const char *v[] = { "" }; size_t l = strs(a, v, 1); n = put_rpc(pkt, 0x41, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(result_strings(0x41, r) == 2); CHECK(!strcmp(r[1], "RandomPw#2345!x")); CHECK(!strcmp(g_stored_ap_pass, r[1])); fixture("ap_pass_random");

    // 5) AP password: too short -> INVALID, nothing stored, no restart
    reset_world(true);
    { const char *v[] = { "short" }; size_t l = strs(a, v, 1); n = put_rpc(pkt, 0x41, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(last_error() == 1); CHECK(g_stored_ap_pass[0] == 0); CHECK(g_restarts == 0);

    // 6) AP password: malformed (declared length longer than the block)
    reset_world(true);
    a[0] = 40; a[1] = 'x'; n = put_rpc(pkt, 0x41, a, 2); run(pkt, n);
    CHECK(last_error() == 1); CHECK(g_stored_ap_pass[0] == 0);

    // 7) WIFI + AP password, success
    reset_world(true);
    { const char *v[] = { "HomeNet", "wifipass123", "HotspotPw#789" }; size_t l = strs(a, v, 3); n = put_rpc(pkt, 0x42, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(result_strings(0x42, r) == 3); CHECK(!strcmp(r[0], "http://192.168.1.77/")); CHECK(!strcmp(r[2], "HotspotPw#789"));
    CHECK(g_saved); CHECK(!strcmp(g_cfg.sta_ssid, "HomeNet")); CHECK(!strcmp(g_cfg.sta_pass, "wifipass123"));
    CHECK(!strcmp(g_cfg.ap_pass, "HotspotPw#789")); CHECK(g_restarts == 1); fixture("wifi_ap");

    // 8) WIFI + AP password, empty AP password -> random
    reset_world(true);
    { const char *v[] = { "HomeNet", "wifipass123", "" }; size_t l = strs(a, v, 3); n = put_rpc(pkt, 0x42, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(result_strings(0x42, r) == 3); CHECK(!strcmp(r[2], "RandomPw#2345!x")); CHECK(!strcmp(g_cfg.ap_pass, "RandomPw#2345!x")); fixture("wifi_ap_random");

    // 9) WIFI + AP password, join FAILS -> nothing stored, no restart, error 3
    reset_world(false);
    { const char *v[] = { "HomeNet", "wrong", "HotspotPw#789" }; size_t l = strs(a, v, 3); n = put_rpc(pkt, 0x42, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(last_error() == 3); CHECK(!g_saved); CHECK(g_restarts == 0); CHECK(g_stored_ap_pass[0] == 0); fixture("wifi_ap_fail");

    // 10) WIFI + AP password, bad hotspot password -> INVALID before touching Wi-Fi
    reset_world(true);
    { const char *v[] = { "HomeNet", "wifipass123", "abc" }; size_t l = strs(a, v, 3); n = put_rpc(pkt, 0x42, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(last_error() == 1); CHECK(!g_saved); CHECK(g_joined_ssid[0] == 0);

    // 11) trailing bytes after the third string are rejected
    reset_world(true);
    { const char *v[] = { "HomeNet", "wifipass123", "HotspotPw#789" }; size_t l = strs(a, v, 3); a[l++] = 7; n = put_rpc(pkt, 0x42, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(last_error() == 1); CHECK(!g_saved);

    // 12) unknown command still answers UNKNOWN_RPC_COMMAND
    reset_world(true);
    n = put_rpc(pkt, 0x55, NULL, 0); run(pkt, n);
    CHECK(last_error() == 2);

    // 13) corrupted checksum is rejected with INVALID
    reset_world(true);
    n = put_rpc(pkt, 0x40, NULL, 0); pkt[n - 2] ^= 0xFF; run(pkt, n);
    CHECK(last_error() == 1 && result_strings(0x40, r) == -1);


    // 14) quick connect: fourth string "1" arms the flag, only after a successful join
    reset_world(true);
    { const char *v[] = { "HomeNet", "wifipass123", "HotspotPw#789", "1" }; size_t l = strs(a, v, 4); n = put_rpc(pkt, 0x42, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(result_strings(0x42, r) == 3); CHECK(g_auto_ble == true); CHECK(g_saved); CHECK(g_restarts == 1); fixture("wifi_ap_quick");

    // 15) fourth string "0" (or none) leaves it off
    reset_world(true);
    { const char *v[] = { "HomeNet", "wifipass123", "HotspotPw#789", "0" }; size_t l = strs(a, v, 4); n = put_rpc(pkt, 0x42, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(g_auto_ble == false && g_saved);
    reset_world(true); g_auto_ble = true;     // a stale flag is cleared by a normal 0x42 without quick
    { const char *v[] = { "HomeNet", "wifipass123", "HotspotPw#789" }; size_t l = strs(a, v, 3); n = put_rpc(pkt, 0x42, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(g_auto_ble == false);

    // 16) quick connect + FAILED join: the flag must NOT be armed
    reset_world(false);
    { const char *v[] = { "HomeNet", "wrong", "HotspotPw#789", "1" }; size_t l = strs(a, v, 4); n = put_rpc(pkt, 0x42, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(last_error() == 3); CHECK(g_auto_ble == false); CHECK(!g_saved);

    // 17) fourth string too long / junk after it -> INVALID, nothing stored
    reset_world(true);
    { const char *v[] = { "HomeNet", "wifipass123", "HotspotPw#789", "yes!" }; size_t l = strs(a, v, 4); n = put_rpc(pkt, 0x42, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(last_error() == 1); CHECK(!g_saved); CHECK(g_auto_ble == false);

    // 18) Bluetooth status: nothing scanned yet
    reset_world(true);
    n = put_rpc(pkt, 0x43, NULL, 0); run(pkt, n);
    CHECK(result_strings(0x43, r) == 3); CHECK(!strcmp(r[0], "0")); CHECK(!strcmp(r[2], "0")); fixture("ble_empty");

    // 19) Bluetooth status: armed + two controllers
    reset_world(true); g_auto_ble = true;
    g_ble.scanned = true; g_ble.count = 2;
    strcpy(g_ble.dev[0].addr, "AA:BB:CC:00:11:22"); strcpy(g_ble.dev[0].name, "SF-GGS-1A"); g_ble.dev[0].rssi = -61;
    strcpy(g_ble.dev[1].addr, "AA:BB:CC:00:11:33"); strcpy(g_ble.dev[1].name, "SF-GGS-2B"); g_ble.dev[1].rssi = -78;
    strcpy(g_ble.last_result, "Quick connect: 2 of 2 controller(s) connected to the hotspot, still visible over Bluetooth. ");
    n = put_rpc(pkt, 0x43, NULL, 0); run(pkt, n);
    CHECK(result_strings(0x43, r) == 5); CHECK(!strcmp(r[0], "1")); CHECK(!strcmp(r[2], "2"));
    CHECK(!strcmp(r[3], "AA:BB:CC:00:11:22|SF-GGS-1A|-61")); CHECK(!strcmp(r[4], "AA:BB:CC:00:11:33|SF-GGS-2B|-78"));
    CHECK(g_restarts == 0); fixture("ble_found");

    // 20) a scan that was never completed is not reported as "0 found"
    reset_world(true);
    g_ble.scanned = false; g_ble.count = 3;
    n = put_rpc(pkt, 0x43, NULL, 0); run(pkt, n);
    CHECK(result_strings(0x43, r) == 3); CHECK(!strcmp(r[2], "0"));

    // 21) restart command: answers first, then restarts through the supervisor
    reset_world(true);
    n = put_rpc(pkt, 0x44, NULL, 0); run(pkt, n);
    CHECK(result_strings(0x44, r) == 1); CHECK(!strcmp(r[0], "1")); CHECK(g_restarts == 1); CHECK(g_sv_why == SV_WHY_USER); fixture("restart");

    // 22) diagnostics: a bridge that last crashed because of a watchdog, 2 restarts in a row
    reset_world(true);
    strcpy(g_sv.reset_text, "Task watchdog"); g_sv.boots = 17; g_sv.crash_streak = 2; g_sv.crashes = 5; g_sv.safe_mode = false;
    g_sv.last_sv_why = SV_WHY_HEARTBEAT; strcpy(g_sv.last_sv_task, "config_poll");
    g_sv.uptime_s = 3600; g_sv.heap_free = 91234; g_sv.heap_min = 61000; g_sv.heap_largest = 40960;
    n = put_rpc(pkt, 0x45, NULL, 0); run(pkt, n);
    CHECK(result_strings(0x45, r) == 11);
    CHECK(!strcmp(r[0], "Task watchdog")); CHECK(!strcmp(r[1], "17")); CHECK(!strcmp(r[2], "2")); CHECK(!strcmp(r[3], "5"));
    CHECK(!strcmp(r[4], "0")); CHECK(!strcmp(r[5], "1")); CHECK(!strcmp(r[6], "config_poll"));
    CHECK(!strcmp(r[7], "3600")); CHECK(!strcmp(r[8], "91234")); CHECK(!strcmp(r[9], "61000")); CHECK(!strcmp(r[10], "40960"));
    CHECK(g_restarts == 0); fixture("diag");

    // 23) diagnostics in safe mode
    reset_world(true);
    strcpy(g_sv.reset_text, "Crash"); g_sv.crash_streak = 3; g_sv.safe_mode = true;
    n = put_rpc(pkt, 0x45, NULL, 0); run(pkt, n);
    CHECK(result_strings(0x45, r) == 11); CHECK(!strcmp(r[4], "1")); CHECK(!strcmp(r[2], "3")); fixture("diag_safe");

    // 24) the listener registers one heartbeat and never ends on its own: after the last byte it just waits
    reset_world(true); g_slots = 0;
    n = put_rpc(pkt, 0x02, NULL, 0); run(pkt, n);
    CHECK(g_slots == 1);

    // 25) fault injection: needs the confirmation word AND a known fault, nothing else triggers it
    reset_world(true);
    { const char *v[] = { "CRASH-TEST", "taskwdt" }; size_t l = strs(a, v, 2); n = put_rpc(pkt, 0x46, a, (uint8_t)l); }
    run(pkt, n);
    CHECK(result_strings(0x46, r) == 1); CHECK(!strcmp(r[0], "taskwdt")); CHECK(!strcmp(g_fault, "taskwdt"));
    reset_world(true);
    { const char *v[] = { "crash-test", "panic" }; size_t l = strs(a, v, 2); n = put_rpc(pkt, 0x46, a, (uint8_t)l); }
    run(pkt, n); CHECK(last_error() == 1); CHECK(g_fault[0] == 0);          // wrong word
    reset_world(true);
    { const char *v[] = { "CRASH-TEST", "format-flash" }; size_t l = strs(a, v, 2); n = put_rpc(pkt, 0x46, a, (uint8_t)l); }
    run(pkt, n); CHECK(last_error() == 1); CHECK(g_fault[0] == 0);          // unknown fault
    reset_world(true);
    { const char *v[] = { "panic" }; size_t l = strs(a, v, 1); n = put_rpc(pkt, 0x46, a, (uint8_t)l); }
    run(pkt, n); CHECK(last_error() == 1); CHECK(g_fault[0] == 0);          // word missing
    reset_world(true);
    { const char *v[] = { "CRASH-TEST", "panic", "x" }; size_t l = strs(a, v, 3); n = put_rpc(pkt, 0x46, a, (uint8_t)l); }
    run(pkt, n); CHECK(last_error() == 1); CHECK(g_fault[0] == 0);          // trailing junk
    fprintf(fx, "}\n"); fclose(fx);
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("ALL host tests passed\n");
    return 0;
}
