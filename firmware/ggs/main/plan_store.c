#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sb_config.h"
#include "plan_text.h"
#include "plan_store.h"
#include "device_cache.h"

static const char *TAG = "plan_store";

// ---------------------------------------------------------------------------
// Layout in the "storage" partition
//
//   0x00000  BLE job log
//   0x10000  plan templates (40 x 4 KB)
//   0x38000  long plans: SB_MAX_DEVICES areas of 10 sectors (40 KB) each
//
// Area: sector 0 = header (magic, mac, info, the stage order), sectors
// 1..9 = data, three 1360-byte slots each (27 slots). A stage keeps its
// slot; the header's order[] lists slots by start date. Rewriting one
// stage erases one data sector (holding three stages, rewritten together)
// plus the header -- a few KB of RAM at most, never the whole plan.
// ---------------------------------------------------------------------------
#define AREA_BASE      0x38000
#define AREA_SECTORS   10
#define AREA_SIZE      (AREA_SECTORS * 0x1000)
#define SLOT_SIZE      1360
#define SLOTS_PER_SEC  3
#define HDR_MAGIC      0x504C4E32u   // "PLN2"

typedef struct {
    uint32_t magic;
    char     mac[16];
    uint8_t  active, running;
    uint16_t count;
    int16_t  window;
    char     name[32];
    uint8_t  order[PLAN_STORE_MAX_STAGES];   // slot of the i-th stage
} hdr_t;

typedef struct {
    uint16_t len;          // 0 = free
    char     json[SLOT_SIZE - 2];
} slot_t;

static SemaphoreHandle_t s_lock;

static const esp_partition_t *part(void)
{
    static const esp_partition_t *p;
    if (!p) p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    return p;
}

static void lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }

// A header read from flash is trusted only when its counts and slot numbers
// are in range: a torn write with an intact magic would otherwise index
// past the order/used arrays.
static bool hdr_sane(const hdr_t *h)
{
    if (h->count > PLAN_STORE_MAX_STAGES) return false;
    if (h->window < -1 || h->window > PLAN_STORE_MAX_STAGES) return false;
    for (int i = 0; i < h->count; i++)
        if (h->order[i] >= PLAN_STORE_MAX_STAGES) return false;
    return true;
}

// Headers in RAM, so the read paths (relay task, web feed) never wait for
// the flash or for a long erase under the lock. Filled on first use,
// updated by every header write. valid: 0 unknown, 1 read.
static hdr_t   s_hdr[SB_MAX_DEVICES];
static uint8_t s_hdr_state[SB_MAX_DEVICES];
static portMUX_TYPE s_hdr_mux = portMUX_INITIALIZER_UNLOCKED;

// The area of this controller: the one whose header carries its MAC, else
// (create) the first free one.
static int area_for(const char *mac, bool create, hdr_t *h)
{
    const esp_partition_t *p = part();
    if (!p || !mac || !mac[0]) return -1;
    int free_a = -1;
    for (int a = 0; a < SB_MAX_DEVICES; a++) {
        size_t off = AREA_BASE + (size_t)a * AREA_SIZE;
        if (off + AREA_SIZE > p->size) break;
        if (esp_partition_read(p, off, h, sizeof(*h)) != ESP_OK) continue;
        if (h->magic == HDR_MAGIC && !hdr_sane(h)) {
            ESP_LOGW(TAG, "Plan header %d is damaged -- ignored", a);
            h->magic = 0;
        }
        portENTER_CRITICAL(&s_hdr_mux);
        s_hdr[a] = *h;
        s_hdr_state[a] = 1;
        portEXIT_CRITICAL(&s_hdr_mux);
        if (h->magic == HDR_MAGIC && strncmp(h->mac, mac, sizeof(h->mac)) == 0) return a;
        if (h->magic != HDR_MAGIC && free_a < 0) free_a = a;
    }
    if (!create || free_a < 0) return -1;
    memset(h, 0, sizeof(*h));
    h->magic = HDR_MAGIC;
    strncpy(h->mac, mac, sizeof(h->mac) - 1);
    h->window = -1;
    return free_a;
}

static bool hdr_write(int a, const hdr_t *h)
{
    device_cache_bump();   // the web page shows the long plan
    size_t off = AREA_BASE + (size_t)a * AREA_SIZE;
    portENTER_CRITICAL(&s_hdr_mux);
    s_hdr[a] = *h;
    s_hdr_state[a] = 1;
    portEXIT_CRITICAL(&s_hdr_mux);
    if (esp_partition_erase_range(part(), off, 0x1000) != ESP_OK) return false;
    return esp_partition_write(part(), off, h, sizeof(*h)) == ESP_OK;
}

static void hdr_forget(int a)
{
    portENTER_CRITICAL(&s_hdr_mux);
    memset(&s_hdr[a], 0, sizeof(s_hdr[a]));
    s_hdr_state[a] = 1;
    portEXIT_CRITICAL(&s_hdr_mux);
}

static size_t slot_off(int a, int slot)
{
    int sec = 1 + slot / SLOTS_PER_SEC;
    return AREA_BASE + (size_t)a * AREA_SIZE + (size_t)sec * 0x1000 + (size_t)(slot % SLOTS_PER_SEC) * SLOT_SIZE;
}

static size_t slot_read(int a, int slot, char *out, size_t n)
{
    uint16_t len = 0;
    size_t off = slot_off(a, slot);
    if (esp_partition_read(part(), off, &len, 2) != ESP_OK) return 0;
    if (len == 0 || len == 0xFFFF || len >= n || len > SLOT_SIZE - 2) return 0;
    if (esp_partition_read(part(), off + 2, out, len) != ESP_OK) return 0;
    out[len] = '\0';
    return len;
}

// Rewrites one data sector with slot `slot` set to json (NULL = free).
static bool slot_write(int a, int slot, const char *json)
{
    int sec = 1 + slot / SLOTS_PER_SEC;
    size_t sec_off = AREA_BASE + (size_t)a * AREA_SIZE + (size_t)sec * 0x1000;
    uint8_t *buf = malloc(SLOTS_PER_SEC * SLOT_SIZE);
    if (!buf) return false;
    bool ok = esp_partition_read(part(), sec_off, buf, SLOTS_PER_SEC * SLOT_SIZE) == ESP_OK;
    if (ok) {
        uint8_t *s = buf + (slot % SLOTS_PER_SEC) * SLOT_SIZE;
        memset(s, 0xFF, SLOT_SIZE);
        if (json) {
            uint16_t len = (uint16_t)strlen(json);
            memcpy(s, &len, 2);
            memcpy(s + 2, json, len);
        } else {
            uint16_t z = 0;
            memcpy(s, &z, 2);
        }
        // Slots never written are 0xFF; keep them reading as free.
        ok = esp_partition_erase_range(part(), sec_off, 0x1000) == ESP_OK &&
             esp_partition_write(part(), sec_off, buf, SLOTS_PER_SEC * SLOT_SIZE) == ESP_OK;
    }
    free(buf);
    return ok;
}

static int free_slot(const hdr_t *h)
{
    bool used[PLAN_STORE_MAX_STAGES] = {0};
    for (int i = 0; i < h->count; i++) used[h->order[i]] = true;
    for (int s = 0; s < PLAN_STORE_MAX_STAGES; s++) if (!used[s]) return s;
    return -1;
}

// ---------------------------------------------------------------------------

bool plan_store_info(const char *mac, plan_store_info_t *out)
{
    if (!mac || !mac[0]) return false;
    hdr_t h;
    int a = -1;
    bool all_known = true;
    // RAM first: no flash, no lock (called from the relay task per frame).
    portENTER_CRITICAL(&s_hdr_mux);
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        if (!s_hdr_state[i]) { all_known = false; continue; }
        if (s_hdr[i].magic == HDR_MAGIC && strncmp(s_hdr[i].mac, mac, sizeof(s_hdr[i].mac)) == 0) {
            h = s_hdr[i];
            a = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_hdr_mux);
    if (a < 0 && !all_known) {
        lock();
        a = area_for(mac, false, &h);
        unlock();
    }
    if (a < 0) return false;
    if (out) {
        out->active = h.active;
        out->running = h.running;
        out->count = h.count;
        out->window = h.window;
        memcpy(out->name, h.name, sizeof(out->name));
        out->name[sizeof(out->name) - 1] = '\0';
    }
    return true;
}

size_t plan_store_get(const char *mac, int i, char *out, size_t n)
{
    hdr_t h;
    size_t l = 0;
    lock();
    int a = area_for(mac, false, &h);
    if (a >= 0 && i >= 0 && i < h.count) l = slot_read(a, h.order[i], out, n);
    unlock();
    return l;
}

int plan_store_put(const char *mac, const char *stage, char *err, size_t errsz)
{
    size_t sl = strlen(stage);
    if (sl < 2 || sl > SLOT_SIZE - 2) {
        snprintf(err, errsz, "Stage too large (%u bytes, max %u)", (unsigned)sl, SLOT_SIZE - 2);
        return -1;
    }
    long id = jtext_int(stage, sl, "stageId", -1);
    long st = jtext_int(stage, sl, "startDate", -1);
    long en = jtext_int(stage, sl, "endDate", -1);
    if (id < 0 || st <= 0 || en < st) { snprintf(err, errsz, "Not a valid stage"); return -1; }
    long co = jtext_int(stage, sl, "color", 1);
    if (co < 1 || co > 5) { snprintf(err, errsz, "Colour must be 1-5"); return -1; }

    char *buf = malloc(SLOT_SIZE);
    if (!buf) { snprintf(err, errsz, "Out of memory"); return -1; }
    hdr_t h;
    int result = -1;
    lock();
    int a = area_for(mac, true, &h);
    if (a < 0) { snprintf(err, errsz, "No room for another controller's plan"); goto out; }

    // Same stageId: replace in place (its slot), remove from the order.
    int slot = -1;
    for (int i = 0; i < h.count; i++) {
        if (!slot_read(a, h.order[i], buf, SLOT_SIZE)) continue;
        if (jtext_int(buf, strlen(buf), "stageId", -2) == id) {
            slot = h.order[i];
            memmove(&h.order[i], &h.order[i + 1], h.count - i - 1);
            h.count--;
            break;
        }
    }
    if (slot < 0) {
        if (h.count >= PLAN_STORE_MAX_STAGES) {
            snprintf(err, errsz, "The plan already has %d stages (maximum)", PLAN_STORE_MAX_STAGES);
            goto out;
        }
        slot = free_slot(&h);
        if (slot < 0) { snprintf(err, errsz, "No free stage slot"); goto out; }
    }
    // Position by startDate; reject overlaps with neighbours.
    int pos = h.count;
    for (int i = 0; i < h.count; i++) {
        if (!slot_read(a, h.order[i], buf, SLOT_SIZE)) continue;
        size_t bl = strlen(buf);
        long os = jtext_int(buf, bl, "startDate", 0), oe = jtext_int(buf, bl, "endDate", 0);
        if (!(en < os || st > oe)) {
            const char *lv; size_t ll;
            char lab[40] = "another stage";
            if (jtext_find(buf, bl, "label", &lv, &ll) && ll >= 2) snprintf(lab, sizeof(lab), "%.*s", (int)(ll - 2 < 36 ? ll - 2 : 36), lv + 1);
            snprintf(err, errsz, "Overlaps the stage \"%s\"", lab);
            goto out;
        }
        if (st < os && pos == h.count) pos = i;
    }
    if (!slot_write(a, slot, stage)) { snprintf(err, errsz, "Flash write failed"); goto out; }
    memmove(&h.order[pos + 1], &h.order[pos], h.count - pos);
    h.order[pos] = (uint8_t)slot;
    h.count++;
    if (!hdr_write(a, &h)) { snprintf(err, errsz, "Flash write failed"); goto out; }
    result = pos;
out:
    unlock();
    free(buf);
    return result;
}

bool plan_store_delete(const char *mac, long stage_id)
{
    char *buf = malloc(SLOT_SIZE);
    if (!buf) return false;
    hdr_t h;
    bool ok = false;
    lock();
    int a = area_for(mac, false, &h);
    for (int i = 0; a >= 0 && i < h.count; i++) {
        if (!slot_read(a, h.order[i], buf, SLOT_SIZE)) continue;
        if (jtext_int(buf, strlen(buf), "stageId", -2) != stage_id) continue;
        slot_write(a, h.order[i], NULL);
        memmove(&h.order[i], &h.order[i + 1], h.count - i - 1);
        h.count--;
        ok = hdr_write(a, &h);
        break;
    }
    unlock();
    free(buf);
    return ok;
}

bool plan_store_clear(const char *mac)
{
    hdr_t h;
    lock();
    int a = area_for(mac, false, &h);
    if (a >= 0) hdr_forget(a);
    bool ok = a >= 0 &&
        esp_partition_erase_range(part(), AREA_BASE + (size_t)a * AREA_SIZE, AREA_SIZE) == ESP_OK;
    unlock();
    if (ok) ESP_LOGI(TAG, "Long plan for %s deleted", mac);
    return ok;
}

static bool set_field(const char *mac, int which, int v)
{
    hdr_t h;
    lock();
    int a = area_for(mac, which != 2, &h);
    bool ok = false;
    if (a >= 0) {
        if (which == 0) h.running = v ? 1 : 0;
        else if (which == 1) h.active = v ? 1 : 0;
        else h.window = (int16_t)v;
        ok = hdr_write(a, &h);
    }
    unlock();
    return ok;
}

int plan_store_deactivate_all(void)
{
    const esp_partition_t *p = part();
    if (!p) return 0;
    int n = 0;
    hdr_t h;
    lock();
    for (int a = 0; a < SB_MAX_DEVICES; a++) {
        size_t off = AREA_BASE + (size_t)a * AREA_SIZE;
        if (off + AREA_SIZE > p->size) break;
        if (esp_partition_read(p, off, &h, sizeof(h)) != ESP_OK) continue;
        if (h.magic != HDR_MAGIC || !hdr_sane(&h)) {
            if (h.magic == HDR_MAGIC) ESP_LOGW(TAG, "Plan header %d is damaged -- ignored", a);
            hdr_forget(a);
            continue;
        }
        if (h.active) {
            h.active = 0;
            if (hdr_write(a, &h)) n++;
        } else {
            portENTER_CRITICAL(&s_hdr_mux);
            s_hdr[a] = h;
            s_hdr_state[a] = 1;
            portEXIT_CRITICAL(&s_hdr_mux);
        }
    }
    unlock();
    return n;
}

bool plan_store_set_running(const char *mac, bool running) { return set_field(mac, 0, running); }
bool plan_store_set_active(const char *mac, bool active)   { return set_field(mac, 1, active); }
bool plan_store_set_window(const char *mac, int first)     { return set_field(mac, 2, first); }

int plan_store_current(const char *mac, int today)
{
    char *buf = malloc(SLOT_SIZE);
    if (!buf) return -1;
    hdr_t h;
    int cur = -1;
    lock();
    int a = area_for(mac, false, &h);
    for (int i = 0; a >= 0 && i < h.count; i++) {
        if (!slot_read(a, h.order[i], buf, SLOT_SIZE)) continue;
        size_t bl = strlen(buf);
        long e = jtext_int(buf, bl, "endDate", 0);
        if (today <= e) { cur = i; break; }    // running today, or the next one
    }
    unlock();
    free(buf);
    return cur;
}

int plan_store_import(const char *mac, const char *plan, size_t len)
{
    const char *arr;
    size_t al;
    if (!jtext_find(plan, len, "stage", &arr, &al) || arr[0] != '[') return 0;
    plan_store_clear(mac);
    char err[64];
    int n = 0;
    size_t i = 1;
    while (i < al) {
        while (i < al && (arr[i] == ' ' || arr[i] == ',')) i++;
        if (i >= al || arr[i] == ']') break;
        size_t vl = jtext_value_len(arr + i, al - i);
        if (!vl) break;
        char *s = malloc(vl + 1);
        if (!s) break;
        memcpy(s, arr + i, vl);
        s[vl] = '\0';
        if (plan_store_put(mac, s, err, sizeof(err)) >= 0) n++;
        free(s);
        i += vl;
    }
    return n;
}
