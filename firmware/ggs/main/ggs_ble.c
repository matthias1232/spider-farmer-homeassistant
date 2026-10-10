#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_attr.h"
#include "esp_bt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/aes.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#include "cJSON.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "provisioning.h"
#include "device_registry.h"
#include "ggs_ble.h"
#include "supervisor.h"
#include "esp_rom_sys.h"

static const char *TAG = "ggs_ble";

// GATT layout of the GGS setup service.
#define GGS_SVC      0x00FF
#define GGS_NOTIFY   0xFF01
#define GGS_WRITE    0xFF02

// ---------------------------------------------------------------------------
// Keys per product code, from the app: AES key and IV. The GGS controller
// is pcode 1004 and is tried first; the rest are tried in turn when a
// reply does not decrypt, so other Spider Farmer devices work as well.
// ---------------------------------------------------------------------------
typedef struct { int pcode; const char *key; const char *iv; } ggs_key_t;
static const ggs_key_t KEYS[] = {
    { 1004, "iVi6D24KxbrvXUuO", "RnWokNEvKW6LcWJg" },
    { 1002, "mKli62mtym9j6Odi", "h411AfTnVVusvsjE" },
    { 1003, "lVIlATSlxaS1btfd", "84Rf7SUkinfvxNlc" },
    { 1005, "BkJu61kLt3afuogT", "2AKVNUbU4mvU3Elt" },
    { 1007, "FIUz0N1xrmaaso61", "75YdgtITdMfiyS5x" },
    { 1008, "v04Y436txRWeHd6w", "2pci13UbdjPR1blE" },
    {    0, "bt3MSw3YDM0gRdEP", "J4G0M9dX1f1v3fXr" },
};
#define KEY_COUNT ((int)(sizeof(KEYS) / sizeof(KEYS[0])))

// ---------------------------------------------------------------------------
// State that survives the restarts into and out of the Bluetooth boot.
// RTC slow memory keeps its contents across esp_restart(); a magic word
// tells a real power-on (garbage) from a deliberate restart.
// ---------------------------------------------------------------------------
// Changes with every layout change of ble_rtc_t, so RTC contents left by
// an older firmware are discarded instead of read misaligned.
#define BLE_RTC_MAGIC 0x47475305u   // "GGS" + layout version 5
#define BLE_RTC_MAGIC_V BLE_RTC_MAGIC
typedef enum { OP_NONE = 0, OP_SCAN = 1, OP_PROVISION = 2, OP_UNPAIR = 3 } op_t;
typedef struct {
    uint32_t magic;
    uint32_t op;                 // requested operation for the next boot
    uint32_t from_ble;           // this restart is the return from a Bluetooth boot
    uint32_t bind;               // OP_PROVISION: also bind the controller (setDevActive)
    char     target[18];         // device for OP_PROVISION / OP_UNPAIR
    ggs_ble_status_t st;         // results, shown by the next normal boot
} ble_rtc_t;
static RTC_NOINIT_ATTR ble_rtc_t s_rtc;

static SemaphoreHandle_t s_lock = NULL;
static bool s_released = false;   // normal boot gave Bluetooth's RAM to the heap
static bool s_boot_scan = false;  // this Bluetooth boot is the automatic start-up scan
#define s_st (s_rtc.st)
static volatile bool s_synced = false;
static int  s_scan_s = 8;
static char s_target[18];

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_h_write, s_h_notify;
static SemaphoreHandle_t s_evt = NULL;
static volatile int s_evt_rc = 0;

// Reassembly of the controller's (possibly chunked) reply. A message is
// identified by the ciphertext CRC in header bytes 8..9.
static uint8_t *s_rx = NULL;
static uint16_t s_rx_total = 0;
static uint16_t s_rx_got = 0;
static uint16_t s_rx_crc = 0;
static volatile bool s_rx_done = false;

// Decrypted messages are matched against what the provisioning step waits
// for: the reply carrying our msgId, and the latest getSysSta push.
static const ggs_key_t *s_key = NULL;          // key that decrypted the last message
static char s_wait_msgid[24];                   // msgId we wait for, "" = none
static volatile bool s_got_reply = false;
static volatile int  s_reply_code = 0;
static volatile int  s_wifi_connected = -1;     // from getSysSta, -1 = unknown
static volatile int  s_wifi_rssi = 0;
static char s_uid[24];                          // account uid the controller reports
static void on_message(void);

// Notification byte stream. With an MTU below the frame size the
// controller splits one frame over several notifications, so frames are
// cut out of this stream by their length field, not per notification.
// Heap only, and only in the Bluetooth boot: no static DRAM in normal use.
#define ACC_SIZE 1200
static uint8_t *s_acc = NULL;
static size_t   s_acc_n = 0;
static volatile int s_disc_reason = 0;   // last disconnect reason, 0 = none
static int s_attempt = 1;                // current connect attempt (parameter choice)
// Diagnostics for the trace: where do notifications get lost?
static volatile uint32_t s_n_notify, s_n_bytes, s_n_frames, s_n_badcrc, s_n_msgs, s_n_undec;
static volatile int s_first_len = -1;
static uint8_t s_first_head[8];

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---------------------------------------------------------------------------
// Detailed job log
//
// Written step by step during the Bluetooth boot (heap there is plentiful),
// then stored in the unused "storage" flash partition just before the
// restart, so the normal boot can serve it at /ble/log. The hotspot
// password is masked in every line; the ciphertext is shown as sent.
// ---------------------------------------------------------------------------
#define BLOG_SIZE   (24 * 1024)
#define BLOG_MAGIC  0x474C4F47u   // "GLOG"
static char *s_blog = NULL;
static size_t s_blog_n = 0;
static bool s_blog_full = false;
static SemaphoreHandle_t s_blog_lock = NULL;
static int64_t s_t0 = 0;
static char s_secret[72] = "";    // masked wherever it would appear
static bool s_verbose = true;     // full detail for the current command
static int  s_cmd_no = 0;

static void blog_put(const char *s, size_t l)
{
    if (!s_blog || s_blog_full) return;
    if (s_blog_n + l + 64 >= BLOG_SIZE) {
        static const char full[] = "... log full, the rest is omitted\n";
        memcpy(s_blog + s_blog_n, full, sizeof(full) - 1);
        s_blog_n += sizeof(full) - 1;
        s_blog_full = true;
        return;
    }
    memcpy(s_blog + s_blog_n, s, l);
    s_blog_n += l;
}

// Appends text with the secret replaced by "*****".
static void blog_put_masked(const char *s, size_t l)
{
    size_t sl = strlen(s_secret);
    while (l) {
        const char *hit = NULL;
        if (sl) {
            for (size_t i = 0; i + sl <= l; i++)
                if (memcmp(s + i, s_secret, sl) == 0) { hit = s + i; break; }
        }
        if (!hit) { blog_put(s, l); return; }
        blog_put(s, (size_t)(hit - s));
        blog_put("*****", 5);
        l -= (size_t)(hit - s) + sl;
        s = hit + sl;
    }
}

static void blog_stamp(void)
{
    char ts[16];
    int64_t ms = (esp_timer_get_time() - s_t0) / 1000;
    int n = snprintf(ts, sizeof(ts), "[%3d.%03d] ", (int)(ms / 1000), (int)(ms % 1000));
    blog_put(ts, (size_t)n);
}

static void blog(const char *fmt, ...)
{
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
    ESP_LOGI(TAG, "%s", line);
    if (s_blog_lock) xSemaphoreTake(s_blog_lock, portMAX_DELAY);
    blog_stamp();
    blog_put_masked(line, (size_t)n);
    blog_put("\n", 1);
    if (s_blog_lock) xSemaphoreGive(s_blog_lock);
}

// A long text (JSON) on its own indented line, cut at max characters.
static void blog_text(const char *indent, const char *s, size_t max)
{
    size_t l = strlen(s);
    if (s_blog_lock) xSemaphoreTake(s_blog_lock, portMAX_DELAY);
    blog_put("          ", 10);
    blog_put(indent, strlen(indent));
    blog_put_masked(s, l > max ? max : l);
    if (l > max) {
        char more[32];
        int n = snprintf(more, sizeof(more), " ... (+%u chars)", (unsigned)(l - max));
        blog_put(more, (size_t)n);
    }
    blog_put("\n", 1);
    if (s_blog_lock) xSemaphoreGive(s_blog_lock);
}

// Hex dump, 16 bytes per line with offsets.
static void blog_hex(const uint8_t *d, size_t n)
{
    static const char HX[] = "0123456789abcdef";
    if (s_blog_lock) xSemaphoreTake(s_blog_lock, portMAX_DELAY);
    for (size_t o = 0; o < n; o += 16) {
        char line[80];
        int p = snprintf(line, sizeof(line), "            %04x  ", (unsigned)o);
        for (size_t i = o; i < o + 16 && i < n; i++) {
            line[p++] = HX[d[i] >> 4];
            line[p++] = HX[d[i] & 15];
            line[p++] = ' ';
        }
        line[p++] = '\n';
        blog_put(line, (size_t)p);
    }
    if (s_blog_lock) xSemaphoreGive(s_blog_lock);
}

static void blog_begin(void)
{
    if (!s_blog_lock) s_blog_lock = xSemaphoreCreateMutex();
    free(s_blog);
    s_blog = malloc(BLOG_SIZE);
    s_blog_n = 0;
    s_blog_full = false;
    s_t0 = esp_timer_get_time();
}

// Stores the log in flash: [magic][length][text].
static void blog_save(void)
{
    if (!s_blog) return;
    const esp_partition_t *pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (pt && pt->size >= BLOG_SIZE + 8) {
        uint32_t hdr[2] = { BLOG_MAGIC, (uint32_t)s_blog_n };
        size_t erase = ((BLOG_SIZE + 8 + 4095) / 4096) * 4096;
        if (esp_partition_erase_range(pt, 0, erase) == ESP_OK &&
            esp_partition_write(pt, 8, s_blog, s_blog_n) == ESP_OK)
            esp_partition_write(pt, 0, hdr, sizeof(hdr));
    }
    free(s_blog);
    s_blog = NULL;
}

size_t ggs_ble_log_size(void)
{
    const esp_partition_t *pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_ANY, "storage");
    uint32_t hdr[2];
    if (!pt || esp_partition_read(pt, 0, hdr, sizeof(hdr)) != ESP_OK) return 0;
    if (hdr[0] != BLOG_MAGIC || hdr[1] > BLOG_SIZE) return 0;
    return hdr[1];
}

size_t ggs_ble_log_read(size_t off, char *buf, size_t n)
{
    size_t total = ggs_ble_log_size();
    if (off >= total) return 0;
    if (n > total - off) n = total - off;
    const esp_partition_t *pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!pt || esp_partition_read(pt, 8 + off, buf, n) != ESP_OK) return 0;
    return n;
}

static const char *hci_reason(int r)
{
    switch (r & 0xFF) {
        case 0x08: return "supervision timeout (link lost)";
        case 0x13: return "the controller closed the connection";
        case 0x16: return "closed by the bridge";
        case 0x22: return "LL response timeout";
        case 0x3E: return "connection failed to be established";
        default:   return "see Bluetooth HCI error codes";
    }
}

// Appends one line to the trace kept in RTC memory, so the next normal
// boot can show what the Bluetooth boot did (its own log is gone by then).
static void trace(const char *fmt, ...)
{
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    ESP_LOGI(TAG, "%s", line);
    size_t used = strnlen(s_st.trace, sizeof(s_st.trace));
    if (used + strlen(line) + 2 >= sizeof(s_st.trace)) return;
    snprintf(s_st.trace + used, sizeof(s_st.trace) - used, "%s%s", used ? "\n" : "", line);
}

static void set_result(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lock();
    strcpy(s_st.last_result, buf);
    unlock();
    ESP_LOGI(TAG, "%s", buf);
}

// ---------------------------------------------------------------------------
// Frame codec
// ---------------------------------------------------------------------------

static uint16_t crc16_modbus(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static uint16_t get16(const uint8_t *p)   { return (uint16_t)(p[0] << 8 | p[1]); }

// Encrypts json and wraps it in a single frame. Returns the frame length.
static size_t frame_encode(const ggs_key_t *k, const char *json, uint8_t *out, size_t outsz)
{
    size_t n = strlen(json);
    size_t padded = (n / 16 + 1) * 16;
    if (20 + padded + 2 > outsz) return 0;
    uint8_t *ct = out + 20;
    memcpy(ct, json, n);
    memset(ct + n, (int)(padded - n), padded - n);

    mbedtls_aes_context aes;
    unsigned char iv[16];
    memcpy(iv, k->iv, 16);
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, (const unsigned char *)k->key, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, iv, ct, ct);
    mbedtls_aes_free(&aes);

    // Header layout verified against the controller (it answers only when
    // bytes 8..9 carry the CRC of the whole ciphertext; anything else is
    // dropped without a word):
    //   0 AAAA | 2 0003 | 4 len-8 | 6 0002 | 8 CRC16(ciphertext) | 10 0000 |
    //   12 total | 14 0000 | 16 chunk offset | 18 chunk length
    size_t frame_len = 20 + padded + 2;
    out[0] = 0xAA; out[1] = 0xAA;
    put16(out + 2, 0x0003);
    put16(out + 4, (uint16_t)(frame_len - 8));
    put16(out + 6, 0x0002);
    put16(out + 8, crc16_modbus(ct, padded));
    put16(out + 10, 0);
    put16(out + 12, (uint16_t)padded);   // total length
    put16(out + 14, 0);
    put16(out + 16, 0);                  // offset of this chunk
    put16(out + 18, (uint16_t)padded);   // length of this chunk
    put16(out + 20 + padded, crc16_modbus(out, 20 + padded));

    if (s_verbose) {
        blog("   Encrypt: AES-128-CBC, key set of pcode %d, PKCS#7 padding %u -> %u bytes",
             k->pcode, (unsigned)n, (unsigned)padded);
        blog("   Ciphertext (%u bytes):", (unsigned)padded);
        blog_hex(ct, padded);
        blog("   Frame header: magic AAAA, type 0003, length %u, opcode 0002,",
             (unsigned)(frame_len - 8));
        blog("                 ciphertext CRC16 %04X, total %u, chunk offset 0, chunk length %u,",
             get16(out + 8), (unsigned)padded, (unsigned)padded);
        blog("                 frame CRC16 %04X (Modbus, over header + chunk)",
             get16(out + 20 + padded));
        blog("   Complete frame (%u bytes) as written:", (unsigned)frame_len);
        blog_hex(out, frame_len);
    }
    return frame_len;
}

// Decrypts with one key pair; true when the result is JSON.
static bool try_decrypt(const ggs_key_t *k, const uint8_t *ct, size_t n, char *out, size_t outsz)
{
    if (n == 0 || n % 16 || n >= outsz) return false;
    mbedtls_aes_context aes;
    unsigned char iv[16];
    memcpy(iv, k->iv, 16);
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, (const unsigned char *)k->key, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, n, iv, ct, (unsigned char *)out);
    mbedtls_aes_free(&aes);
    uint8_t pad = (uint8_t)out[n - 1];
    size_t len = (pad >= 1 && pad <= 16) ? n - pad : n;
    out[len] = '\0';
    return out[0] == '{';
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

static int gap_cb(struct ble_gap_event *ev, void *arg);

static void record_found(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return;
    char name[32] = "";
    if (f.name && f.name_len) {
        size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
        memcpy(name, f.name, n);
    }
    // Manufacturer data: company 0xFFFF, then "EC 03 00", then the Wi-Fi
    // MAC (seen as ff ff ec 03 00 d0 cf 13 7a 60 b8 ...).
    bool has_mfg = f.mfg_data && f.mfg_data_len >= 11 &&
                   f.mfg_data[0] == 0xFF && f.mfg_data[1] == 0xFF;
    // The app's filter is the name "SF-GGS-..."; the name can arrive in the
    // scan response only, so a matching manufacturer block also counts.
    if (strncmp(name, "SF-GGS", 6) != 0 && !has_mfg) return;

    char a[18];
    snprintf(a, sizeof(a), "%02X:%02X:%02X:%02X:%02X:%02X",
             d->addr.val[5], d->addr.val[4], d->addr.val[3],
             d->addr.val[2], d->addr.val[1], d->addr.val[0]);
    lock();
    int idx = -1;
    for (int i = 0; i < s_st.count; i++)
        if (strcmp(s_st.dev[i].addr, a) == 0) { idx = i; break; }
    if (idx < 0 && s_st.count < GGS_BLE_MAX_FOUND) idx = s_st.count++;
    if (idx >= 0) {
        ggs_ble_dev_t *g = &s_st.dev[idx];
        strcpy(g->addr, a);
        g->addr_type = d->addr.type;
        if (name[0]) strncpy(g->name, name, sizeof(g->name) - 1);
        g->rssi = d->rssi;
        if (has_mfg) {
            snprintf(g->wifi_mac, sizeof(g->wifi_mac), "%02X%02X%02X%02X%02X%02X",
                     f.mfg_data[5], f.mfg_data[6], f.mfg_data[7],
                     f.mfg_data[8], f.mfg_data[9], f.mfg_data[10]);
            g->pcode = (uint16_t)(f.mfg_data[2] | (f.mfg_data[3] << 8));
            g->flags = f.mfg_data[4];
            g->has_flags = true;
        }
    }
    unlock();
}

static void do_scan(void)
{
    lock();
    s_st.count = 0;
    memset(s_st.dev, 0, sizeof(s_st.dev));
    unlock();

    struct ble_gap_disc_params p = {0};
    p.passive = 0;           // active: the name comes in the scan response
    p.filter_duplicates = 0; // a later scan response still adds the name
    p.itvl = 0x50;
    p.window = 0x30;
    uint8_t own;
    ble_hs_id_infer_auto(0, &own);
    blog("Scanning for %d s (active scan: asks every device for its name) ...", s_scan_s);
    xSemaphoreTake(s_evt, 0);
    int rc = ble_gap_disc(own, s_scan_s * 1000, &p, gap_cb, NULL);
    if (rc != 0) {
        set_result("Scan could not start (rc=%d)", rc);
        blog("   Scan could not start (rc=%d)", rc);
        return;
    }
    xSemaphoreTake(s_evt, pdMS_TO_TICKS(s_scan_s * 1000 + 3000));
    lock();
    // Entries seen only by their manufacturer block but without the
    // SF-GGS name are dropped: other vendors use company 0xFFFF too.
    int w = 0;
    for (int i = 0; i < s_st.count; i++)
        if (strncmp(s_st.dev[i].name, "SF-GGS", 6) == 0) s_st.dev[w++] = s_st.dev[i];
    s_st.count = w;
    s_st.scanned = true;

    unlock();
    set_result("Scan finished: %d GGS controller(s) found", w);
    blog("   %d GGS controller(s) found:", w);
    for (int i = 0; i < w; i++) {
        const ggs_ble_dev_t *g = &s_st.dev[i];
        blog("   - %s \"%s\" %d dBm, Wi-Fi MAC %s, pcode %u, flags 0x%02x (%s%s%s)",
             g->addr, g->name, g->rssi, g->wifi_mac[0] ? g->wifi_mac : "?", g->pcode, g->flags,
             g->flags & GGS_FLAG_ACTIVE ? "bound to an account " : "",
             g->flags & GGS_FLAG_WIFI ? "on Wi-Fi " : "not on Wi-Fi ",
             g->flags & GGS_FLAG_MQTT ? "cloud up" : "");
    }
}

// ---------------------------------------------------------------------------
// Connect, discover, write, collect the reply
// ---------------------------------------------------------------------------

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        uint16_t u = ble_uuid_u16(&chr->uuid.u);
        if (u == GGS_WRITE)  s_h_write = chr->val_handle;
        if (u == GGS_NOTIFY) s_h_notify = chr->val_handle;
        return 0;
    }
    s_evt_rc = (err->status == BLE_HS_EDONE) ? 0 : err->status;
    xSemaphoreGive(s_evt);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) {
        // Used for the by-UUID and the all-services discovery alike.
        if (ble_uuid_u16(&svc->uuid.u) == GGS_SVC) {
            s_svc_start = svc->start_handle;
            s_svc_end = svc->end_handle;
        }
        return 0;
    }
    s_evt_rc = (err->status == BLE_HS_EDONE) ? 0 : err->status;
    xSemaphoreGive(s_evt);
    return 0;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *err,
                    struct ble_gatt_attr *attr, void *arg)
{
    s_evt_rc = err->status;
    xSemaphoreGive(s_evt);
    return 0;
}

static uint16_t s_h_cccd;

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0 && dsc) {
        if (!s_h_cccd && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16)
            s_h_cccd = dsc->handle;
        return 0;
    }
    xSemaphoreGive(s_evt);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    xSemaphoreGive(s_evt);
    return 0;
}

// A complete message arrived: decrypt it (with the known key, else find
// the key that yields JSON) and pick out what provisioning waits for.
static void on_message(void)
{
    s_n_msgs++;
    char *txt = malloc((size_t)s_rx_total + 1);
    if (!txt) return;
    bool ok = s_key && try_decrypt(s_key, s_rx, s_rx_total, txt, (size_t)s_rx_total + 1);
    for (int i = 0; i < KEY_COUNT && !ok; i++) {
        if (try_decrypt(&KEYS[i], s_rx, s_rx_total, txt, (size_t)s_rx_total + 1)) {
            s_key = &KEYS[i];
            ok = true;
        }
    }
    if (!ok) {
        s_n_undec++;
        blog("<- Message of %u bytes could not be decrypted with any known key", (unsigned)s_rx_total);
        free(txt);
        return;
    }
    cJSON *j = cJSON_Parse(txt);
    {
        // Log: our replies in full, pushes (status the controller sends on
        // its own every few seconds) in full the first time, then short.
        const char *mm = cJSON_GetStringValue(cJSON_GetObjectItem(j, "method"));
        const char *mi = cJSON_GetStringValue(cJSON_GetObjectItem(j, "msgId"));
        bool ours = mi && s_wait_msgid[0] && strcmp(mi, s_wait_msgid) == 0;
        static int n_devsta = 0, n_syssta = 0;
        bool full = ours ? s_verbose : true;
        if (!ours && mm && strcmp(mm, "getDevSta") == 0) full = n_devsta++ < 1;
        else if (!ours && mm && strcmp(mm, "getSysSta") == 0) full = n_syssta++ < 1;
        blog("<- %s: %s, %u bytes ciphertext, decrypted with pcode %d key%s",
             ours ? "Reply" : "Status push", mm ? mm : "?", (unsigned)s_rx_total, s_key->pcode,
             ours ? " -- this is the answer to our command" : "");
        if (full) blog_text("", txt, 1400);
        else blog("   (same content as shown above, not repeated)");
    }
    if (j) {
        const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(j, "method"));
        const char *msgid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "msgId"));
        const char *uid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "uid"));
        if (uid) { strncpy(s_uid, uid, sizeof(s_uid) - 1); s_uid[sizeof(s_uid) - 1] = '\0'; }
        if (s_wait_msgid[0] && msgid && strcmp(msgid, s_wait_msgid) == 0) {
            cJSON *code = cJSON_GetObjectItem(j, "code");
            s_reply_code = cJSON_IsNumber(code) ? code->valueint : 0;
            s_got_reply = true;
        }
        if (method && strcmp(method, "getSysSta") == 0) {
            cJSON *sys = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "data"), "sys");
            cJSON *wifi = cJSON_GetObjectItem(sys, "wifi");
            cJSON *ic = cJSON_GetObjectItem(wifi, "isConnect");
            if (cJSON_IsNumber(ic)) {
                s_wifi_connected = ic->valueint;
                cJSON *r = cJSON_GetObjectItem(wifi, "rssi");
                s_wifi_rssi = cJSON_IsNumber(r) ? r->valueint : 0;
            }
        }
        cJSON_Delete(j);
    }
    free(txt);
}

// One notification carries one frame: header, ciphertext chunk, CRC.
static void rx_frame(const uint8_t *f, int len)
{
    if (len < 22 || f[0] != 0xAA || f[1] != 0xAA) return;
    uint16_t pcrc = get16(f + 8), total = get16(f + 12), off = get16(f + 16), clen = get16(f + 18);
    s_n_frames++;
    if (20 + clen + 2 != len || crc16_modbus(f, len - 2) != get16(f + len - 2)) {
        s_n_badcrc++;
        blog("<- Frame of %d bytes dropped: CRC or length wrong", len);
        return;
    }
    if (s_verbose)
        blog("<- Notification on 0xFF01: frame %d bytes, ciphertext CRC %04X, chunk %u+%u of %u bytes",
             len, pcrc, off, clen, total);
    if (total == 0 || total > 2048 || (uint32_t)off + clen > total) return;
    // One message = one ciphertext CRC. A different one starts a new
    // message and drops a stale partial one.
    if (!s_rx || s_rx_crc != pcrc || s_rx_total != total || s_rx_done) {
        free(s_rx);
        s_rx = malloc(total);
        s_rx_total = total;
        s_rx_crc = pcrc;
        s_rx_got = 0;
        s_rx_done = false;
        if (!s_rx) return;
    }
    memcpy(s_rx + off, f + 20, clen);
    s_rx_got += clen;
    if (s_rx_got >= s_rx_total && crc16_modbus(s_rx, s_rx_total) == s_rx_crc) {
        s_rx_done = true;
        on_message();
    }
}

// Appends one notification to the stream and hands every complete frame
// to rx_frame. The length field (bytes 4..5) is the frame length minus 8.
static void rx_bytes(const uint8_t *d, int len)
{
    if (!s_acc) s_acc = malloc(ACC_SIZE);
    if (!s_acc) return;
    if (s_acc_n + (size_t)len > ACC_SIZE) s_acc_n = 0;   // garbage: start over
    memcpy(s_acc + s_acc_n, d, len);
    s_acc_n += len;
    for (;;) {
        // Resynchronise on the magic, dropping anything in front of it.
        size_t s = 0;
        while (s + 1 < s_acc_n && !(s_acc[s] == 0xAA && s_acc[s + 1] == 0xAA)) s++;
        if (s) { memmove(s_acc, s_acc + s, s_acc_n - s); s_acc_n -= s; }
        if (s_acc_n < 22) return;
        size_t flen = (size_t)get16(s_acc + 4) + 8;
        if (flen < 22 || flen > ACC_SIZE) {   // not a frame: skip this magic
            memmove(s_acc, s_acc + 1, s_acc_n - 1); s_acc_n -= 1;
            continue;
        }
        if (s_acc_n < flen) return;           // wait for the rest
        rx_frame(s_acc, (int)flen);
        memmove(s_acc, s_acc + flen, s_acc_n - flen);
        s_acc_n -= flen;
    }
}

static int gap_cb(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        record_found(&ev->disc);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        xSemaphoreGive(s_evt);
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        s_evt_rc = ev->connect.status;
        if (ev->connect.status == 0) s_conn = ev->connect.conn_handle;
        xSemaphoreGive(s_evt);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_disc_reason = ev->disconnect.reason;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        blog("Link closed: reason 0x%x, %s", ev->disconnect.reason, hci_reason(ev->disconnect.reason));
        // Wakes a step still waiting on this link, so it fails at once.
        s_evt_rc = BLE_HS_ENOTCONN;
        xSemaphoreGive(s_evt);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        int len = OS_MBUF_PKTLEN(ev->notify_rx.om);
        if (len <= 0 || len > 600) return 0;
        uint8_t *buf = malloc(len);
        if (!buf) return 0;
        ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, len, NULL);
        s_n_notify++;
        s_n_bytes += len;
        if (s_first_len < 0) {
            s_first_len = len;
            memcpy(s_first_head, buf, len < 8 ? len : 8);
        }
        rx_bytes(buf, len);
        free(buf);
        return 0;
    }
    default:
        return 0;
    }
}

static bool wait_evt(int ms) { return xSemaphoreTake(s_evt, pdMS_TO_TICKS(ms)) == pdTRUE; }

// Writes one encrypted command (it must carry msgId), then waits up to
// wait_ms for the controller's reply with that msgId. Returns the reply
// code (200 = ok), 0 when nothing came back, -1 when the write failed.
static int send_cmd(const ggs_key_t *k, const char *json, const char *msgid, int wait_ms)
{
    uint8_t frame[400];
    s_cmd_no++;
    const char *m1 = strstr(json, "\"method\":\"");
    char meth[24] = "?";
    if (m1) { sscanf(m1 + 10, "%23[^\"]", meth); }
    blog("-> Command #%d: %s (msgId %s)", s_cmd_no, meth, msgid ? msgid : "-");
    if (s_verbose) {
        blog("   Plain JSON (%u bytes):", (unsigned)strlen(json));
        blog_text("", json, 400);
    }
    size_t n = frame_encode(k, json, frame, sizeof(frame));
    if (!n) { blog("   Frame does not fit (%u bytes JSON)", (unsigned)strlen(json)); return -1; }
    strncpy(s_wait_msgid, msgid ? msgid : "", sizeof(s_wait_msgid) - 1);
    s_wait_msgid[sizeof(s_wait_msgid) - 1] = '\0';
    s_got_reply = false;
    s_reply_code = 0;

    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return -1;
    blog("   Writing the frame to 0xFF02 (handle 0x%04x), GATT write with response ...", s_h_write);
    xSemaphoreTake(s_evt, 0);
    int rc;
    if (n <= (size_t)ble_att_mtu(s_conn) - 3) {
        rc = ble_gattc_write_flat(s_conn, s_h_write, frame, n, on_write, NULL);
    } else {
        // Frame larger than one ATT packet (small MTU): a long write.
        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, n);
        rc = om ? ble_gattc_write_long(s_conn, s_h_write, 0, om, on_write, NULL) : BLE_HS_ENOMEM;
    }
    if (rc != 0 || !wait_evt(6000) || s_evt_rc != 0) {
        trace("write %u B failed rc=%d st=%d mtu=%u",
              (unsigned)n, rc, s_evt_rc, ble_att_mtu(s_conn));
        blog("   GATT write to handle 0x%04x FAILED (rc=%d, ATT status=%d, MTU %u)",
             s_h_write, rc, s_evt_rc, ble_att_mtu(s_conn));
        s_wait_msgid[0] = '\0';
        return -1;
    }
    blog("   Write acknowledged by the controller (%u bytes, %s)%s",
         (unsigned)n, n <= (size_t)ble_att_mtu(s_conn) - 3 ? "one ATT packet" : "long write",
         s_got_reply ? " -- its reply had already arrived, see above" : "");

    int64_t t0 = esp_timer_get_time();
    for (int t = 0; t < wait_ms && !s_got_reply; t += 50) vTaskDelay(pdMS_TO_TICKS(50));
    s_wait_msgid[0] = '\0';
    if (s_got_reply)
        blog("   => Command #%d %s: code %d%s", s_cmd_no, meth, s_reply_code,
             s_reply_code == 200 ? " (ok)" : "");
    else
        blog("   => Command #%d %s: no reply within %d ms", s_cmd_no, meth, wait_ms);
    (void)t0;
    trace("#%d %s -> %s", s_cmd_no, meth, s_got_reply ? (s_reply_code == 200 ? "200 ok" : "error") : "no reply");
    return s_got_reply ? s_reply_code : 0;
}

static void make_msgid(char *out, size_t n)
{
    // Milliseconds like the app; the bridge has no clock in the Bluetooth
    // boot, so uptime plus a random tail keeps it unique.
    snprintf(out, n, "%llu%04u", (unsigned long long)(esp_timer_get_time() / 1000),
             (unsigned)(esp_random() % 10000));
}

static void disconnect(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 30 && s_conn != BLE_HS_CONN_HANDLE_NONE; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// One attempt: connect and find the GGS service. Returns false (with a
// result message) when anything is missing.
static bool connect_once(void)
{
    ble_addr_t peer = {0};
    unsigned v[6];
    if (sscanf(s_target, "%x:%x:%x:%x:%x:%x", &v[5], &v[4], &v[3], &v[2], &v[1], &v[0]) != 6) {
        set_result("Bad address %s", s_target);
        return false;
    }
    for (int i = 0; i < 6; i++) peer.val[i] = (uint8_t)v[i];
    peer.type = BLE_ADDR_PUBLIC;
    lock();
    for (int i = 0; i < s_st.count; i++)
        if (strcmp(s_st.dev[i].addr, s_target) == 0) peer.type = s_st.dev[i].addr_type;
    unlock();

    uint8_t own;
    ble_hs_id_infer_auto(0, &own);
    s_disc_reason = 0;
    s_evt_rc = 0;
    // Explicit, relaxed parameters: the defaults (short supervision
    // timeout) end in HCI 0x3E "failed to be established" with this
    // controller now and then. 30-50 ms interval, 6 s supervision.
    struct ble_gap_conn_params cp = {
        .scan_itvl = 0x40, .scan_window = 0x40,
        .itvl_min = 24, .itvl_max = 40, .latency = 0,
        .supervision_timeout = 600, .min_ce_len = 0, .max_ce_len = 0,
    };
    blog("Connecting to %s (%s address), interval 30-50 ms, supervision timeout 6 s ...",
         s_target, peer.type == BLE_ADDR_PUBLIC ? "public" : "random");
    // Alternate between the relaxed parameters and the stack's defaults:
    // the GGS has dropped either with 0x3E on some days, never both.
    xSemaphoreTake(s_evt, 0);
    int rc = ble_gap_connect(own, &peer, 10000, (s_attempt % 2) ? NULL : &cp, gap_cb, NULL);
    if (rc != 0 || !wait_evt(12000) || s_evt_rc != 0 || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        if (rc == 0 && s_conn == BLE_HS_CONN_HANDLE_NONE) ble_gap_conn_cancel();
        set_result("Could not connect to %s (rc=%d, status=%d) -- in range and not paired with a phone?",
                   s_target, rc, s_evt_rc);
        blog("   Connection failed (rc=%d, status=%d)", rc, s_evt_rc);
        return false;
    }
    blog("   Link established (connection handle %u). No pairing or bonding -- the GGS "
         "setup service is open; its data is protected by AES instead.", s_conn);
    // The controller sets up its side right after the link comes up;
    // asking too early is what made 1.9.1 report "no setup service".
    vTaskDelay(pdMS_TO_TICKS(600));

    // A larger MTU so a whole frame fits in one write.
    blog("Negotiating ATT MTU (asking for %d) ...", CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU);
    xSemaphoreTake(s_evt, 0);
    if (ble_gattc_exchange_mtu(s_conn, on_mtu, NULL) == 0) wait_evt(3000);
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        set_result("%s dropped the connection (reason 0x%x)", s_target, s_disc_reason);
        blog("   Controller dropped the link: reason 0x%x, %s", s_disc_reason, hci_reason(s_disc_reason));
        return false;
    }
    blog("   MTU %u -- a whole frame fits in one packet (the GGS needs at least ~430)",
         ble_att_mtu(s_conn));
    blog("Discovering the setup service 0x00FF ...");

    // By UUID first; some stacks answer that oddly, so a full service
    // discovery is the fallback.
    ble_uuid16_t svc = BLE_UUID16_INIT(GGS_SVC);
    s_svc_start = s_svc_end = 0;
    xSemaphoreTake(s_evt, 0);
    rc = ble_gattc_disc_svc_by_uuid(s_conn, &svc.u, on_svc, NULL);
    if (rc == 0) wait_evt(6000);
    if (!s_svc_start && s_conn != BLE_HS_CONN_HANDLE_NONE) {
        xSemaphoreTake(s_evt, 0);
        rc = ble_gattc_disc_all_svcs(s_conn, on_svc, NULL);
        if (rc == 0) wait_evt(8000);
    }
    if (!s_svc_start) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE)
            set_result("%s dropped the connection during discovery (reason 0x%x)", s_target, s_disc_reason);
        else
            set_result("%s: setup service 0x00FF not found (rc=%d)", s_target, rc);
        return false;
    }
    blog("   Found: handles 0x%04x-0x%04x", s_svc_start, s_svc_end);
    s_h_write = s_h_notify = 0;
    xSemaphoreTake(s_evt, 0);
    rc = ble_gattc_disc_all_chrs(s_conn, s_svc_start, s_svc_end, on_chr, NULL);
    if (rc != 0 || !wait_evt(6000) || !s_h_write || !s_h_notify) {
        set_result("%s: setup characteristics not found (rc=%d)", s_target, rc);
        blog("   Characteristics 0xFF01/0xFF02 not found (rc=%d)", rc);
        return false;
    }
    blog("   0xFF02 (commands, write) = handle 0x%04x, 0xFF01 (replies, notify) = handle 0x%04x",
         s_h_write, s_h_notify);
    // Subscribe to replies. The CCCD is not necessarily right after the
    // value handle (on the GGS it is value+2), so find it.
    s_h_cccd = 0;
    xSemaphoreTake(s_evt, 0);
    rc = ble_gattc_disc_all_dscs(s_conn, s_h_notify, s_svc_end, on_dsc, NULL);
    if (rc == 0) wait_evt(4000);
    if (!s_h_cccd) s_h_cccd = s_h_notify + 1;
    uint8_t on[2] = { 0x01, 0x00 };
    s_evt_rc = 0;
    xSemaphoreTake(s_evt, 0);
    rc = ble_gattc_write_flat(s_conn, s_h_cccd, on, 2, on_write, NULL);
    if (rc != 0 || !wait_evt(3000) || s_evt_rc != 0) {
        set_result("%s: subscribing to replies failed (cccd 0x%04x, rc=%d, status=%d)",
                   s_target, s_h_cccd, rc, s_evt_rc);
        blog("   Subscribing failed (CCCD 0x%04x, rc=%d, status=%d)", s_h_cccd, rc, s_evt_rc);
        return false;
    }
    blog("Subscribed to replies: wrote 01 00 to the CCCD (handle 0x%04x). The controller now "
         "pushes its status every few seconds and answers commands here.", s_h_cccd);
    trace("connected, MTU %u", ble_att_mtu(s_conn));
    return true;
}

// A few attempts: the first connection after a scan fails now and then.
static bool connect_target(void)
{
    for (int attempt = 1; attempt <= 8; attempt++) {
        s_attempt = attempt;
        if (attempt > 1) blog("Retry %d of 8 (%s connection parameters)", attempt,
                              (attempt % 2) ? "default" : "relaxed");
        if (connect_once()) return true;
        trace("attempt %d: %s", attempt, s_st.last_result);
        disconnect();
        vTaskDelay(pdMS_TO_TICKS(1000 + 500 * attempt));
    }
    return false;
}

// The key for this controller: from the product code in its advertisement,
// else from whichever key decrypted a status push, else pcode 1004.
static const ggs_key_t *key_for_target(void)
{
    uint16_t pcode = 0;
    lock();
    for (int i = 0; i < s_st.count; i++)
        if (strcmp(s_st.dev[i].addr, s_target) == 0) pcode = s_st.dev[i].pcode;
    unlock();
    for (int i = 0; i < KEY_COUNT; i++)
        if (pcode && KEYS[i].pcode == pcode) return &KEYS[i];
    return s_key ? s_key : &KEYS[0];
}

// pid = the controller's Wi-Fi MAC: from the advertisement, else the
// Bluetooth address minus 2 (ESP32 address layout).
static void pid_for_target(char *out, size_t n)
{
    out[0] = '\0';
    lock();
    for (int i = 0; i < s_st.count; i++)
        if (strcmp(s_st.dev[i].addr, s_target) == 0 && strlen(s_st.dev[i].wifi_mac) == 12) {
            strncpy(out, s_st.dev[i].wifi_mac, n - 1);
            out[n - 1] = '\0';
        }
    unlock();
    if (out[0]) return;
    unsigned v[6];
    if (sscanf(s_target, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return;
    uint64_t m = 0;
    for (int i = 0; i < 6; i++) m = (m << 8) | (v[i] & 0xFF);
    m = (m - 2) & 0xFFFFFFFFFFFFull;
    snprintf(out, n, "%02X%02X%02X%02X%02X%02X",
             (unsigned)(m >> 40) & 0xFF, (unsigned)(m >> 32) & 0xFF, (unsigned)(m >> 24) & 0xFF,
             (unsigned)(m >> 16) & 0xFF, (unsigned)(m >> 8) & 0xFF, (unsigned)m & 0xFF);
}

// Returns true when the controller accepted the Wi-Fi settings.
static bool do_provision(void)
{
    bool accepted = false;
    sb_prov_cfg_t *p = malloc(sizeof(*p));
    char *json = malloc(400);
    if (!p || !json) {
        set_result("Out of memory");
        goto done;
    }
    sb_prov_load(p);
    if (!p->ap_ssid[0]) { set_result("No hotspot configured"); goto done; }

    // Mask the hotspot password in the detailed log.
    strncpy(s_secret, p->ap_pass, sizeof(s_secret) - 1);
    s_secret[sizeof(s_secret) - 1] = '\0';
    blog("Job: send the Wi-Fi settings of this bridge (SSID \"%s\", password %u characters, "
         "shown as *****) to %s", p->ap_ssid, (unsigned)strlen(p->ap_pass), s_target);

    s_uid[0] = '\0';
    s_wifi_connected = -1;
    if (!connect_target()) goto out;

    // The controller pushes getDevSta/getSysSta on its own once subscribed;
    // the first one tells the account uid and confirms the key.
    blog("Waiting for the first status push (it shows the account uid and confirms the key) ...");
    for (int t = 0; t < 4000 && !s_uid[0]; t += 100) vTaskDelay(pdMS_TO_TICKS(100));
    const ggs_key_t *key = key_for_target();
    char pid[13], msgid[24];
    pid_for_target(pid, sizeof(pid));
    trace("pid %s, key pcode %d, uid \"%s\"", pid, key->pcode, s_uid);
    blog("Envelope: pid %s (the controller's Wi-Fi MAC), uid \"%s\"%s, key set of pcode %d",
         pid, s_uid, s_uid[0] ? "" : " (none reported -- sent empty)", key->pcode);

    // The JSON is built with escaping, so a password with quotes or
    // backslashes still arrives intact. Same envelope as the app over MQTT.
    char ss[70], pw[140];
    size_t a = 0, b = 0;
    for (const char *c = p->ap_ssid; *c && a < sizeof(ss) - 2; c++) {
        if (*c == '"' || *c == '\\') ss[a++] = '\\';
        ss[a++] = *c;
    }
    ss[a] = '\0';
    for (const char *c = p->ap_pass; *c && b < sizeof(pw) - 2; c++) {
        if (*c == '"' || *c == '\\') pw[b++] = '\\';
        pw[b++] = *c;
    }
    pw[b] = '\0';
    make_msgid(msgid, sizeof(msgid));
    snprintf(json, 400, "{\"method\":\"setWifi\",\"params\":{\"ssid\":\"%s\",\"pass\":\"%s\"},"
             "\"msgId\":\"%s\",\"pid\":\"%s\",\"uid\":\"%s\"}", ss, pw, msgid, pid, s_uid);

    int code = send_cmd(key, json, msgid, 4000);
    memset(json, 0, 400);
    if (code < 0) {
        set_result("Sending the Wi-Fi settings to %s failed (Bluetooth write)", s_target);
        goto out;
    }
    if (code == 0) {
        set_result("%s did not acknowledge the Wi-Fi settings", s_target);
        goto out;
    }
    if (code != 200) {
        set_result("%s refused the Wi-Fi settings (code %d)", s_target, code);
        goto out;
    }
    accepted = true;
    blog("setWifi accepted. The controller stores the network and joins it; polling its "
         "status (getSysSta -> data.sys.wifi.isConnect) until it reports the link, at most 30 s.");

    // Watch the controller's own status until it reports the Wi-Fi link.
    s_wifi_connected = -1;
    int polls = 0;
    int64_t poll_end = esp_timer_get_time() + 30LL * 1000 * 1000;
    while (esp_timer_get_time() < poll_end && s_wifi_connected != 1 && s_conn != BLE_HS_CONN_HANDLE_NONE) {
        if (polls) vTaskDelay(pdMS_TO_TICKS(3000));
        make_msgid(msgid, sizeof(msgid));
        snprintf(json, 400, "{\"method\":\"getSysSta\",\"msgId\":\"%s\",\"pid\":\"%s\",\"uid\":\"%s\"}",
                 msgid, pid, s_uid);
        s_verbose = polls++ == 0;     // the encryption steps once, then short
        send_cmd(key, json, msgid, 3000);
        blog("   Wi-Fi state reported: %s", s_wifi_connected == 1 ? "connected"
                                           : s_wifi_connected == 0 ? "not connected yet" : "unknown");
    }
    s_verbose = true;
    if (s_wifi_connected == 1)
        set_result("%s accepted the Wi-Fi settings and joined \"%s\" (signal %d dBm)",
                   s_target, p->ap_ssid, s_wifi_rssi);
    else
        set_result("%s accepted the Wi-Fi settings for \"%s\"; it joins within a minute",
                   s_target, p->ap_ssid);

    // After setWifi the controller stops advertising over Bluetooth on its
    // own, as if paired. Unless the user asked to keep it that way, undo it
    // right here (setDevDeactive), so a phone can still find it.
    if (!s_rtc.bind) {
        const char *uid = s_uid[0] ? s_uid : device_registry_uid(pid);
        blog("Keeping the controller visible: setDevDeactive, so it advertises over Bluetooth "
             "again (it stops advertising by itself after setWifi) ...");
        make_msgid(msgid, sizeof(msgid));
        snprintf(json, 400, "{\"method\":\"setDevDeactive\",\"params\":{\"uid\":\"%s\"},"
                 "\"msgId\":\"%s\",\"pid\":\"%s\",\"uid\":\"%s\"}", uid, msgid, pid, uid);
        int uc = send_cmd(key, json, msgid, 4000);
        char keep[160];
        strncpy(keep, s_st.last_result, sizeof(keep) - 1); keep[sizeof(keep) - 1] = '\0';
        if (uc == 200) set_result("%s -- still visible over Bluetooth", keep);
        else set_result("%s -- unpair afterwards failed (code %d), it may stay invisible", keep, uc);
    }

    // Optional: bind the controller (what the app calls activating it).
    // A bound controller stops advertising over Bluetooth, so no other
    // phone can take it over; "Unpair" (setDevDeactive) undoes this.
    if (s_rtc.bind) {
        // An unbound controller reports no uid: use the one it was last
        // bound to (remembered by the bridge from its MQTT session).
        const char *uid = s_uid[0] ? s_uid : device_registry_uid(pid);
        if (!uid[0]) {
            blog("No account uid known -- the controller stays as setWifi left it (not advertising)");
        } else {
        blog("Binding the controller (setDevActive, uid \"%s\") so it stops advertising ...", uid);
        make_msgid(msgid, sizeof(msgid));
        snprintf(json, 400, "{\"method\":\"setDevActive\",\"params\":{\"uid\":\"%s\",\"uname\":\"devd_%s\"},"
                 "\"msgId\":\"%s\",\"pid\":\"%s\",\"uid\":\"%s\"}", uid, uid, msgid, pid, uid);
        int bc = send_cmd(key, json, msgid, 4000);
        char keep[160];
        strncpy(keep, s_st.last_result, sizeof(keep) - 1); keep[sizeof(keep) - 1] = '\0';
        if (bc == 200) set_result("%s -- and bound (Bluetooth advertising off)", keep);
        else set_result("%s -- binding failed (code %d)", keep, bc);
        }
    }
out:
    blog("Received in total: %u notifications (%u bytes), %u frames (%u dropped), "
         "%u messages (%u not decryptable)",
         (unsigned)s_n_notify, (unsigned)s_n_bytes, (unsigned)s_n_frames, (unsigned)s_n_badcrc,
         (unsigned)s_n_msgs, (unsigned)s_n_undec);
    blog("Result: %s", s_st.last_result);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) blog("Disconnecting ...");
    disconnect();
done:
    free(s_rx); s_rx = NULL;
    free(s_acc); s_acc = NULL; s_acc_n = 0;
    if (p) memset(p, 0, sizeof(*p));
    free(p); free(json);
    return accepted;
}

// Quick connect: after the start-up scan, put every GGS controller that was
// found onto this bridge's hotspot. The installer switches it on for the
// first start after flashing. It is cleared FIRST, so whatever happens below
// (crash, no controller, refusal) the next start is a normal one.
//
// Controllers stay visible over Bluetooth (no setDevActive), so the phone app
// can still pair with them afterwards. Only controllers heard clearly are
// touched: a faint one is probably a neighbour's, and re-pointing somebody
// else's controller at this hotspot would cut it off from its own network.
// -75 dBm is "in the same room or the next one".
#define AUTO_MIN_RSSI (-75)
static void auto_pair_found(void)
{
    if (!sb_prov_auto_ble()) return;
    sb_prov_set_auto_ble(false);

    char addrs[GGS_BLE_MAX_FOUND][18];
    int n = 0;
    lock();
    for (int i = 0; i < s_st.count && n < GGS_BLE_MAX_FOUND; i++) {
        if (s_st.dev[i].rssi < AUTO_MIN_RSSI) continue;
        strncpy(addrs[n], s_st.dev[i].addr, sizeof(addrs[n]) - 1);
        addrs[n][sizeof(addrs[n]) - 1] = '\0';
        n++;
    }
    unlock();

    blog("Quick connect (first start after the installer): %d GGS controller(s) in range", n);
    if (n == 0) {
        set_result("Quick connect: no GGS controller found in range. Put it in pairing mode (not paired "
                   "with a phone) and use \"Scan\" on the bridge's Control page");
        return;
    }
    s_rtc.bind = 0;            // keep them visible over Bluetooth
    int ok = 0;
    char last[160] = "";
    for (int i = 0; i < n; i++) {
        strncpy(s_target, addrs[i], sizeof(s_target) - 1);
        s_target[sizeof(s_target) - 1] = '\0';
        trace("quick connect %d/%d: %s", i + 1, n, s_target);
        if (do_provision()) ok++;
        strncpy(last, s_st.last_result, sizeof(last) - 1);
        last[sizeof(last) - 1] = '\0';
    }
    set_result("Quick connect: %d of %d controller(s) connected to the hotspot, still visible over Bluetooth. %s",
               ok, n, ok ? "" : last);
}

// Unpair over Bluetooth: setDevDeactive. Used when the controller is not
// reachable over the hotspot (otherwise the same command goes over MQTT).
static void do_unpair(void)
{
    char *json = malloc(300);
    if (!json) { set_result("Out of memory"); return; }
    blog("Job: remove the pairing of %s (setDevDeactive) so it advertises again", s_target);
    s_uid[0] = '\0';
    if (!connect_target()) goto out;
    blog("Waiting for the first status push (account uid) ...");
    for (int t = 0; t < 4000 && !s_uid[0]; t += 100) vTaskDelay(pdMS_TO_TICKS(100));
    const ggs_key_t *key = key_for_target();
    char pid[13], msgid[24];
    pid_for_target(pid, sizeof(pid));
    const char *uid = s_uid[0] ? s_uid : device_registry_uid(pid);
    make_msgid(msgid, sizeof(msgid));
    snprintf(json, 300, "{\"method\":\"setDevDeactive\",\"params\":{\"uid\":\"%s\"},"
             "\"msgId\":\"%s\",\"pid\":\"%s\",\"uid\":\"%s\"}", uid, msgid, pid, uid);
    int code = send_cmd(key, json, msgid, 4000);
    if (code == 200)
        set_result("%s unpaired -- it advertises over Bluetooth again and a phone can pair with it", s_target);
    else if (code > 0)
        set_result("%s refused to unpair (code %d)", s_target, code);
    else
        set_result("%s did not answer the unpair command", s_target);
out:
    blog("Result: %s", s_st.last_result);
    disconnect();
    free(s_rx); s_rx = NULL;
    free(s_acc); s_acc = NULL; s_acc_n = 0;
    free(json);
}

// ---------------------------------------------------------------------------
// Boot modes
// ---------------------------------------------------------------------------

static void on_sync(void)   { s_synced = true; }
static void on_reset(int r) { s_synced = false; }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ensure_sync(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_evt = xSemaphoreCreateBinary();
    }
}

bool ggs_ble_boot_check(void)
{
    ensure_sync();
    // A power-on leaves RTC memory with garbage: start from a clean state.
    if (s_rtc.magic != BLE_RTC_MAGIC_V) {
        memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = BLE_RTC_MAGIC;
    }
    // RTC contents written by an older firmware (different struct layout)
    // survive an OTA restart with a valid magic: sanitise the text fields.
    s_st.trace[sizeof(s_st.trace) - 1] = '\0';
    s_st.last_result[sizeof(s_st.last_result) - 1] = '\0';
    for (char *c = s_st.trace; *c; c++)
        if ((unsigned char)*c >= 0x7F || ((unsigned char)*c < 0x20 && *c != '\n')) { s_st.trace[0] = '\0'; break; }
    for (char *c = s_st.last_result; *c; c++)
        if ((unsigned char)*c >= 0x7F || (unsigned char)*c < 0x20) { s_st.last_result[0] = '\0'; break; }
    if (s_st.count < 0 || s_st.count > GGS_BLE_MAX_FOUND) { s_st.count = 0; s_st.scanned = false; }
    if (s_rtc.op == OP_SCAN || s_rtc.op == OP_PROVISION || s_rtc.op == OP_UNPAIR) return true;

    // No job requested for this boot: nothing can be "running". A busy
    // flag left behind by a job that never finished (crash, update during
    // the job) would otherwise block the page forever.
    s_st.pending = false;
    s_st.busy_addr[0] = '\0';

    // Every other start -- power-on, reboot, crash, firmware update --
    // begins with a short Bluetooth scan, so the settings page lists the
    // controllers nearby right away. Not after a Bluetooth boot (that would
    // loop), which is marked in RTC memory.
    if (!s_rtc.from_ble) {
        s_rtc.op = OP_SCAN;
        s_boot_scan = true;
        return true;
    }
    s_rtc.from_ble = 0;

    // Normal boot: Bluetooth is never used, so all of its memory -- the
    // controller's and the host's -- goes back to the heap for the TLS
    // sessions. That is exactly the memory 1.9.0/1.9.1 were missing.
    s_rtc.op = OP_NONE;
    s_st.pending = false;
    s_st.busy_addr[0] = '\0';
    s_released = esp_bt_mem_release(ESP_BT_MODE_BTDM) == ESP_OK;
    return false;
}

bool ggs_ble_memory_released(void) { return s_released; }

// The Bluetooth-only boot is a separate program run: if it stalls (a stack that never answers, a
// connection that neither completes nor fails), nothing else will ever restart the chip into normal
// operation. This one-shot timer does, whatever the Bluetooth code is doing.
#define BLE_BOOT_MAX_S 150
static void ble_boot_timeout(void *arg)
{
    esp_rom_printf("ggs_ble: Bluetooth boot did not finish in %d s -- restarting into normal operation\n", BLE_BOOT_MAX_S);
    sv_restart(SV_WHY_BLE_TIMEOUT);
}

void ggs_ble_run_boot(void)
{
    static esp_timer_handle_t s_boot_guard;
    esp_timer_create_args_t ga = { .callback = ble_boot_timeout, .name = "ble_guard" };
    if (esp_timer_create(&ga, &s_boot_guard) == ESP_OK)
        esp_timer_start_once(s_boot_guard, (uint64_t)BLE_BOOT_MAX_S * 1000000ull);
    op_t op = (op_t)s_rtc.op;
    // Cleared before anything can fail, so a crash in Bluetooth cannot
    // loop the bridge in Bluetooth boots: the next boot is a normal one.
    s_rtc.op = OP_NONE;
    s_rtc.from_ble = 1;
    // Cleared up front as well, so a crash inside this boot cannot leave
    // the page stuck on "Setting up".
    s_st.pending = false;
    if (s_boot_scan) s_scan_s = 6;
    strncpy(s_target, s_rtc.target, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = '\0';
    s_st.trace[0] = '\0';
    blog_begin();
    const char *what = op == OP_SCAN ? "scan for GGS controllers"
                     : op == OP_UNPAIR ? "unpair " : "send Wi-Fi settings to ";
    trace("Bluetooth boot: %s %s", op == OP_SCAN ? "scan" : op == OP_UNPAIR ? "unpair" : "setup", s_target);
    blog("=== Bluetooth-only boot: %s%s%s ===", what, op == OP_SCAN ? "" : s_target,
         s_boot_scan ? " (automatic, at every start of the bridge)" : "");
    blog("Normal operation (hotspot, proxy, Home Assistant) is not running in this boot; "
         "the bridge restarts into it when the job is done.");

    if (nimble_port_init() != ESP_OK) {
        set_result("Bluetooth could not start");
        blog("Bluetooth stack could not start");
    } else {
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.reset_cb = on_reset;
        nimble_port_freertos_init(host_task);
        for (int i = 0; i < 50 && !s_synced; i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_synced) {
            set_result("Bluetooth did not come up");
            blog("Bluetooth controller did not come up");
        } else {
            blog("Bluetooth up (NimBLE, central role, free heap %u bytes)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            if (op == OP_SCAN) {
                do_scan();
                if (s_boot_scan && sb_prov_auto_ble()) {
                    char keep[160];
                    auto_pair_found();
                    strncpy(keep, s_st.last_result, sizeof(keep) - 1);
                    keep[sizeof(keep) - 1] = '\0';
                    // A fresh scan afterwards, so the page shows the new state.
                    vTaskDelay(pdMS_TO_TICKS(500));
                    s_scan_s = 6;
                    blog("Scanning again so the settings page shows the new state ...");
                    do_scan();
                    set_result("%s", keep);
                }
            } else {
                if (op == OP_UNPAIR) do_unpair();
                else do_provision();
                // A fresh scan afterwards, so the page shows the new state.
                vTaskDelay(pdMS_TO_TICKS(500));
                char keep[160];
                strncpy(keep, s_st.last_result, sizeof(keep) - 1);
                keep[sizeof(keep) - 1] = '\0';
                s_scan_s = 6;
                blog("Scanning again so the settings page shows the new state ...");
                do_scan();
                set_result("%s", keep);
            }
        }
    }
    s_st.pending = false;
    s_st.busy_addr[0] = '\0';
    blog("=== Done -- restarting into normal operation (Bluetooth off) ===");
    memset(s_secret, 0, sizeof(s_secret));
    blog_save();
    ESP_LOGW(TAG, "Bluetooth boot done -- restarting into normal operation");
    // Every start begins with this Bluetooth-only boot and restarts BEFORE the normal boot's own
    // esp_ota_mark_app_valid_cancel_rollback() can run. With rollback enabled the bootloader treats any
    // restart of a not-yet-confirmed image as a failed start and returns to the previous firmware, so
    // every over-the-air update used to be undone right here. Reaching this point means the image
    // started, initialised Bluetooth and finished its job: that is the confirmation. A crash or a
    // stall before this line (the guard timer, a panic) still ends in a rollback, as intended.
    esp_ota_mark_app_valid_cancel_rollback();
    vTaskDelay(pdMS_TO_TICKS(300));
    sv_restart(SV_WHY_NONE);
}

static void ble_request_restart(void *arg) { (void)arg; sv_restart(SV_WHY_NONE); }

static bool request(op_t op, const char *addr, uint32_t delay_ms)
{
    ensure_sync();
    if (s_st.pending) return false;
    s_rtc.magic = BLE_RTC_MAGIC;
    s_rtc.op = op;
    if (addr) {
        strncpy(s_rtc.target, addr, sizeof(s_rtc.target) - 1);
        s_rtc.target[sizeof(s_rtc.target) - 1] = '\0';
        strncpy(s_st.busy_addr, addr, sizeof(s_st.busy_addr) - 1);
    }
    s_st.pending = true;
    snprintf(s_st.last_result, sizeof(s_st.last_result), "%s",
             op == OP_SCAN ? "Restarting into Bluetooth to scan..."
           : op == OP_UNPAIR ? "Restarting into Bluetooth to unpair the controller..."
                             : "Restarting into Bluetooth to set up the controller...");
    ESP_LOGW(TAG, "%s", s_st.last_result);
    // Restart from a timer so the web request that asked for it still
    // gets its reply.
    esp_timer_handle_t t;
    esp_timer_create_args_t a = { .callback = ble_request_restart, .name = "ble_rst" };
    if (esp_timer_create(&a, &t) != ESP_OK || esp_timer_start_once(t, (uint64_t)delay_ms * 1000) != ESP_OK) {
        sv_restart(SV_WHY_NONE);
    }
    return true;
}

bool ggs_ble_request_scan(uint32_t delay_ms)
{
    s_scan_s = 8;
    return request(OP_SCAN, NULL, delay_ms);
}

bool ggs_ble_request_provision(const char *addr, bool bind, uint32_t delay_ms)
{
    if (!addr || strlen(addr) != 17) return false;
    s_rtc.bind = bind ? 1 : 0;
    return request(OP_PROVISION, addr, delay_ms);
}

bool ggs_ble_request_unpair(const char *addr, uint32_t delay_ms)
{
    if (!addr || strlen(addr) != 17) return false;
    return request(OP_UNPAIR, addr, delay_ms);
}

void ggs_ble_get_status(ggs_ble_status_t *out)
{
    if (!out) return;
    ensure_sync();
    lock();
    *out = s_st;
    unlock();
}
