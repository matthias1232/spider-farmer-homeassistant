#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "device_registry.h"
#include "device_cache.h"

static const char *TAG = "devices";
#define NVS_NS "sbdev"

static device_entry_t s_devices[DEV_MAX];
static SemaphoreHandle_t s_lock = NULL;

void device_registry_lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void device_registry_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// Everything in a topic or entity id has to survive MQTT and Home
// Assistant unchanged, so anything outside [a-z0-9_] becomes an
// underscore and the result is lowercased.
static void sanitise_slug(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c)) {
            out[o++] = (char)tolower(c);
        } else if (o > 0 && out[o - 1] != '_') {
            out[o++] = '_';
        }
    }
    // No trailing underscore.
    while (o > 0 && out[o - 1] == '_') o--;
    out[o] = '\0';
}

// Defaults when a controller has no name/topic of its own: always carry
// the full Wi-Fi MAC, so every controller stays identifiable.
void device_registry_default_name(const char *mac, char *out, size_t n)
{
    snprintf(out, n, "GGS %s", mac ? mac : "");
}

void device_registry_default_slug(const char *mac, char *out, size_t n)
{
    char raw[DEV_SLUG_LEN];
    snprintf(raw, sizeof(raw), "ggs_%s", mac ? mac : "");
    sanitise_slug(raw, out, n);
}

// Saving the list to NVS is a flash write (tens of ms, occasionally far
// longer when NVS compacts a page). It used to run under the registry
// lock, which the controller relay takes on every status frame -- the
// relay stalled for the whole write. Now persist() (always called with the
// lock held) only marks the list as changed and wakes a small writer task;
// the writer copies the list under the lock and writes the copy without it.
static volatile bool s_dirty = false;
static TaskHandle_t s_writer = NULL;

static void writer_task(void *arg)
{
    (void)arg;
    device_entry_t *snap = malloc(sizeof(s_devices));
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Coalesce a burst (a new device brings uid, zone and calibration).
        vTaskDelay(pdMS_TO_TICKS(300));
        if (!snap) snap = malloc(sizeof(s_devices));
        if (!snap) { ESP_LOGW(TAG, "Could not save the device list (no memory)"); continue; }
        device_registry_lock();
        bool d = s_dirty;
        s_dirty = false;
        memcpy(snap, s_devices, sizeof(s_devices));
        device_registry_unlock();
        if (!d) continue;
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
            ESP_LOGW(TAG, "Could not save the device list");
            continue;
        }
        nvs_set_blob(h, "devices", snap, sizeof(s_devices));
        nvs_commit(h);
        nvs_close(h);
    }
}

void device_registry_flush(void)
{
    if (!s_dirty) return;
    device_entry_t *snap = malloc(sizeof(s_devices));
    if (!snap) return;
    device_registry_lock();
    memcpy(snap, s_devices, sizeof(s_devices));
    s_dirty = false;
    device_registry_unlock();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "devices", snap, sizeof(s_devices));
        nvs_commit(h);
        nvs_close(h);
    }
    free(snap);
}

static void persist(void)
{
    // Every persisted change (new device, rename, calibration, zone) is
    // shown on the web page, so an open page picks it up.
    device_cache_bump();
    s_dirty = true;
    if (s_writer) {
        xTaskNotifyGive(s_writer);
        return;
    }
    // Before the writer exists (early boot): write directly.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Could not save the device list");
        return;
    }
    nvs_set_blob(h, "devices", s_devices, sizeof(s_devices));
    nvs_commit(h);
    nvs_close(h);
    s_dirty = false;
}

void device_registry_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    memset(s_devices, 0, sizeof(s_devices));
    // NVS calls need a few KB of stack; 3 KB plus the snapshot on the heap.
    if (!s_writer) xTaskCreate(writer_task, "devsave", 3072, NULL, 3, &s_writer);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_devices);
        esp_err_t e = nvs_get_blob(h, "devices", NULL, &len);
        if (e == ESP_OK && len == sizeof(s_devices)) {
            nvs_get_blob(h, "devices", s_devices, &len);
        } else if (e == ESP_OK && len > 0 && len < sizeof(s_devices) &&
                   len % DEV_MAX == 0) {
            // A blob from an older firmware with a smaller entry: the
            // entries share their leading layout, so copy them one by one
            // and leave the new tail fields zero.
            size_t old_sz = len / DEV_MAX;
            uint8_t *old = malloc(len);
            if (old && nvs_get_blob(h, "devices", old, &len) == ESP_OK) {
                for (int i = 0; i < DEV_MAX; i++)
                    memcpy(&s_devices[i], old + i * old_sz, old_sz);
                ESP_LOGW(TAG, "Device list migrated from an older layout (%u -> %u bytes per entry)",
                         (unsigned)old_sz, (unsigned)sizeof(s_devices[0]));
            }
            free(old);
        }
        nvs_close(h);
    }

    // Sessions never survive a restart.
    //
    // The whole struct is persisted as one blob, so the session fields
    // come back from NVS with stale values. Clearing them here is what
    // makes "reconnects since boot" mean what it says.
    int n = 0;
    for (int i = 0; i < DEV_MAX; i++) {
        s_devices[i].online = false;
        s_devices[i].discovery_sent = false;
        s_devices[i].connected_since = 0;
        s_devices[i].reconnects = 0;
        s_devices[i].total_online_s = 0;
        s_devices[i].last_reason[0] = '\0';
        if (s_devices[i].in_use) {
            n++;
            // Older firmware used the bare MAC as default name and topic;
            // move those to the current defaults.
            device_entry_t *e = &s_devices[i];
            char bare[DEV_SLUG_LEN];
            sanitise_slug(e->mac, bare, sizeof(bare));
            if (strcmp(e->name, e->mac) == 0 || !e->name[0])
                device_registry_default_name(e->mac, e->name, DEV_NAME_LEN);
            if (strcmp(e->slug, bare) == 0 || !e->slug[0])
                device_registry_default_slug(e->mac, e->slug, DEV_SLUG_LEN);
        }
    }

    if (n > 0) {
        ESP_LOGI(TAG, "%d known controller%s:", n, n == 1 ? "" : "s");
        for (int i = 0; i < DEV_MAX; i++) {
            if (!s_devices[i].in_use) continue;
            ESP_LOGI(TAG, "   %s  %-20s %s",
                     s_devices[i].mac, s_devices[i].slug, s_devices[i].name);
        }
    } else {
        ESP_LOGI(TAG, "No controllers known yet — the first to publish is added");
    }
}

device_entry_t *device_registry_find(const char *mac)
{
    if (!mac || !mac[0]) return NULL;
    for (int i = 0; i < DEV_MAX; i++) {
        if (s_devices[i].in_use && strcasecmp(s_devices[i].mac, mac) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

device_entry_t *device_registry_find_by_slug(const char *slug)
{
    if (!slug || !slug[0]) return NULL;
    for (int i = 0; i < DEV_MAX; i++) {
        if (s_devices[i].in_use && strcmp(s_devices[i].slug, slug) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

device_entry_t *device_registry_resolve(const char *mac)
{
    if (!mac || !mac[0]) return NULL;

    device_registry_lock();

    device_entry_t *e = device_registry_find(mac);
    if (e) {
        device_registry_unlock();
        return e;
    }

    for (int i = 0; i < DEV_MAX; i++) {
        if (s_devices[i].in_use) continue;

        e = &s_devices[i];
        memset(e, 0, sizeof(*e));
        e->in_use = true;
        strncpy(e->mac, mac, DEV_MAC_LEN - 1);

        // Until the user names it, both identifiers are the MAC. That
        // keeps a new controller usable right away instead of waiting
        // for someone to configure it.
        device_registry_default_slug(mac, e->slug, DEV_SLUG_LEN);
        device_registry_default_name(mac, e->name, DEV_NAME_LEN);

        persist();
        ESP_LOGI(TAG, "New controller: %s (slug '%s')", mac, e->slug);

        device_registry_unlock();
        return e;
    }

    ESP_LOGW(TAG, "Controller %s ignored — all %d slots are taken", mac, DEV_MAX);
    device_registry_unlock();
    return NULL;
}

int device_registry_count(void)
{
    int n = 0;
    for (int i = 0; i < DEV_MAX; i++) {
        if (s_devices[i].in_use) n++;
    }
    return n;
}

device_entry_t *device_registry_at(int index)
{
    if (index < 0 || index >= DEV_MAX) return NULL;
    return s_devices[index].in_use ? &s_devices[index] : NULL;
}

bool device_registry_rename(const char *mac, const char *slug, const char *name)
{
    device_registry_lock();

    device_entry_t *e = device_registry_find(mac);
    if (!e) {
        device_registry_unlock();
        return false;
    }

    // Empty (or nothing usable) means the default: "ggs_" + MAC.
    char new_slug[DEV_SLUG_LEN] = "";
    if (slug && slug[0]) sanitise_slug(slug, new_slug, sizeof(new_slug));
    if (!new_slug[0]) device_registry_default_slug(e->mac, new_slug, sizeof(new_slug));

    // Two devices sharing a slug would collide on every topic.
    device_entry_t *clash = device_registry_find_by_slug(new_slug);
    if (clash && clash != e) {
        ESP_LOGW(TAG, "Slug '%s' is already used by %s", new_slug, clash->mac);
        device_registry_unlock();
        return false;
    }

    bool slug_changed = strcmp(e->slug, new_slug) != 0;
    strncpy(e->slug, new_slug, DEV_SLUG_LEN - 1);
    e->slug[DEV_SLUG_LEN - 1] = '\0';

    if (name && name[0]) {
        strncpy(e->name, name, DEV_NAME_LEN - 1);
        e->name[DEV_NAME_LEN - 1] = '\0';
    } else {
        device_registry_default_name(e->mac, e->name, DEV_NAME_LEN);
    }

    // A changed slug means every topic moves, so discovery has to be
    // published again under the new identifiers.
    if (slug_changed) e->discovery_sent = false;

    persist();
    ESP_LOGI(TAG, "Renamed %s to '%s' (slug '%s')", e->mac, e->name, e->slug);

    device_registry_unlock();
    return true;
}

void device_registry_note_uid(const char *mac, const char *uid)
{
    if (!uid || !uid[0]) return;
    device_registry_lock();
    device_entry_t *e = device_registry_find(mac);
    if (e && strncmp(e->uid, uid, sizeof(e->uid) - 1) != 0) {
        strncpy(e->uid, uid, sizeof(e->uid) - 1);
        e->uid[sizeof(e->uid) - 1] = '\0';
        persist();
        ESP_LOGI(TAG, "%s is bound to account %s", mac, e->uid);
    }
    device_registry_unlock();
}

const char *device_registry_uid(const char *mac)
{
    static char out[24];
    out[0] = '\0';
    device_registry_lock();
    device_entry_t *e = device_registry_find(mac);
    if (e) strncpy(out, e->uid, sizeof(out) - 1);
    device_registry_unlock();
    out[sizeof(out) - 1] = '\0';
    return out;
}

bool device_registry_remove(const char *mac)
{
    device_registry_lock();

    device_entry_t *e = device_registry_find(mac);
    if (!e) {
        device_registry_unlock();
        return false;
    }

    ESP_LOGI(TAG, "Forgetting %s ('%s')", e->mac, e->name);
    memset(e, 0, sizeof(*e));
    persist();

    device_registry_unlock();
    return true;
}

// Seconds since boot, the common clock for all session timing.
static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

void device_registry_set_online_why(const char *mac, bool online,
                                    const char *reason)
{
    device_registry_lock();

    device_entry_t *e = device_registry_find(mac);
    if (!e) {
        device_registry_unlock();
        return;
    }

    bool was = e->online;
    e->online = online;
    if (was != online) device_cache_bump();   // the page shows online/offline

    if (online && !was) {
        e->connected_since = uptime_s();
        e->last_reason[0] = '\0';
    } else if (!online && was) {
        // Count the finished session and remember why it ended. A
        // reconnect count that keeps climbing is the clearest sign of
        // an unstable link, even when the device looks fine right now.
        if (e->connected_since) {
            uint32_t now = uptime_s();
            if (now > e->connected_since) {
                e->total_online_s += now - e->connected_since;
            }
        }
        e->connected_since = 0;
        e->reconnects++;

        if (reason && reason[0]) {
            strncpy(e->last_reason, reason, DEV_REASON_LEN - 1);
            e->last_reason[DEV_REASON_LEN - 1] = '\0';
        }
    }

    device_registry_unlock();
}

void device_registry_set_online(const char *mac, bool online)
{
    device_registry_set_online_why(mac, online, NULL);
}

void device_registry_set_cal(const char *mac, const char *field, float value)
{
    device_registry_lock();

    device_entry_t *e = device_registry_find(mac);
    if (e) {
        float *p = NULL;
        if      (strcmp(field, "temp") == 0) p = &e->cal_temp;
        else if (strcmp(field, "humi") == 0) p = &e->cal_humi;
        else if (strcmp(field, "co2")  == 0) p = &e->cal_co2;
        else if (strcmp(field, "ppfd") == 0) p = &e->cal_ppfd;
        // The calibration poll reports the same values every two minutes:
        // only a real change is written to flash (and shown as news).
        if (p && *p != value) {
            *p = value;
            persist();
        }
    }

    device_registry_unlock();
}

void device_registry_get_cal(const char *mac, float *temp, float *humi,
                             float *co2, float *ppfd)
{
    if (temp) *temp = 0;
    if (humi) *humi = 0;
    if (co2)  *co2  = 0;
    if (ppfd) *ppfd = 0;

    device_registry_lock();
    device_entry_t *e = device_registry_find(mac);
    if (e) {
        if (temp) *temp = e->cal_temp;
        if (humi) *humi = e->cal_humi;
        if (co2)  *co2  = e->cal_co2;
        if (ppfd) *ppfd = e->cal_ppfd;
    }
    device_registry_unlock();
}

void device_registry_set_tz(const char *mac, const char *name,
                            const char *posix)
{
    device_registry_lock();

    device_entry_t *e = device_registry_find(mac);
    if (e) {
        bool changed = false;
        if (name && name[0] && strcmp(e->tz_name, name) != 0) {
            strncpy(e->tz_name, name, sizeof(e->tz_name) - 1);
            e->tz_name[sizeof(e->tz_name) - 1] = '\0';
            changed = true;
        }
        if (posix && posix[0] && strcmp(e->tz_posix, posix) != 0) {
            strncpy(e->tz_posix, posix, sizeof(e->tz_posix) - 1);
            e->tz_posix[sizeof(e->tz_posix) - 1] = '\0';
            changed = true;
        }
        if (changed || !e->tz_known) {
            e->tz_known = true;
            // Only written when it actually changed: this arrives with
            // every status frame, and persisting each one would wear the
            // flash out within days.
            if (changed) persist();
        }
    }

    device_registry_unlock();
}

uint32_t device_registry_connected_for(const char *mac)
{
    device_registry_lock();
    device_entry_t *e = device_registry_find(mac);
    uint32_t since = (e && e->online) ? e->connected_since : 0;
    device_registry_unlock();

    if (!since) return 0;
    uint32_t now = uptime_s();
    return now > since ? now - since : 0;
}
