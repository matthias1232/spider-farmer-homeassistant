#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Bluetooth LE: find Spider Farmer GGS controllers and hand them Wi-Fi
//
// Reverse engineered from the Spider Farmer app and verified against a real
// GGS controller:
//
//   Advertising: only while not paired with a phone. Name "SF-GGS-xx",
//                manufacturer data (company 0xFFFF) with the controller's
//                Wi-Fi MAC in bytes 5..10.
//   GATT:        service 0x00FF, write 0xFF02, notify 0xFF01.
//   Frame:       AA AA | 00 03 | len | 00 02 | msgId | 00000000 |
//                total | offset | chunkLen | ciphertext | CRC16/MODBUS (BE)
//   Payload:     JSON, AES-128-CBC, PKCS#7, key/IV chosen by product code.
//   Setup:       {"method":"setWifi","params":{"ssid":..,"pass":..}}
//
// Bluetooth needs memory the controller's TLS session also needs, so it
// never runs during normal operation. A scan or a setup restarts the bridge
// into a short Bluetooth-only boot that does the job, keeps the outcome in
// RTC memory, and restarts back into normal operation. Normal boots release
// all Bluetooth memory to the heap.
// ============================================================================

#define GGS_BLE_MAX_FOUND 8

// Manufacturer data (company 0xFFFF), as advertised:
//   [0..1] FF FF  [2..3] product code, little-endian (EC 03 = 1004)
//   [4]    state flags  [5..10] Wi-Fi MAC
// Flags, from the app's scan screen ("activated / wifi / mqtt"):
#define GGS_FLAG_ACTIVE 0x01   // bound to an account
#define GGS_FLAG_WIFI   0x02   // joined a Wi-Fi network
#define GGS_FLAG_MQTT   0x08   // cloud (MQTT) session up -- via the bridge here

typedef struct {
    char    addr[18];     // "D0:CF:13:7A:60:BA"
    uint8_t addr_type;
    char    name[24];
    int8_t  rssi;
    char    wifi_mac[13]; // from the advertisement, "D0CF137A60B8"
    uint16_t pcode;       // product code from the advertisement, 0 = unknown
    uint8_t flags;        // GGS_FLAG_*
    bool    has_flags;
} ggs_ble_dev_t;

typedef struct {
    bool     pending;        // a Bluetooth boot has been requested
    bool     scanned;        // at least one scan has completed
    int      count;
    ggs_ble_dev_t dev[GGS_BLE_MAX_FOUND];
    char     busy_addr[18];  // device a requested setup will talk to
    char     last_result[160];
    char     trace[512];     // step log of the last Bluetooth boot
} ggs_ble_status_t;

// Call first thing in app_main. Returns true when this is a Bluetooth boot:
// the caller then runs ggs_ble_run_boot() instead of normal operation.
// On a normal boot it releases all Bluetooth memory.
bool ggs_ble_boot_check(void);

// Bluetooth boot: needs only the network stack's absence of TLS sessions.
// Does the requested scan/setup and restarts into normal operation.
void ggs_ble_run_boot(void);

// Requests a scan or a setup: stored, then the bridge restarts into a
// Bluetooth boot after delay_ms (so the web reply gets out first).
bool ggs_ble_request_scan(uint32_t delay_ms);
// bind: after setWifi also bind the controller to the bridge's account
// (setDevActive), which stops its Bluetooth advertising.
bool ggs_ble_request_provision(const char *addr, bool bind, uint32_t delay_ms);
// Removes the binding over Bluetooth (setDevDeactive): the controller
// advertises again so a phone can pair with it.
bool ggs_ble_request_unpair(const char *addr, uint32_t delay_ms);

// Detailed step-by-step log of the last Bluetooth job (stored in flash):
// its length, and a piece of it from offset off.
size_t ggs_ble_log_size(void);
size_t ggs_ble_log_read(size_t off, char *buf, size_t n);

// True when this (normal) boot released all Bluetooth memory to the heap,
// i.e. Bluetooth is off and cannot come on without a restart.
bool ggs_ble_memory_released(void);

// Copies the last results for the web page.
void ggs_ble_get_status(ggs_ble_status_t *out);
