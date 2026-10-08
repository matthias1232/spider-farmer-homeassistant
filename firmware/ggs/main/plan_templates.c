#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "plan_text.h"
#include "sf_normalizer.h"
#include "plan_templates.h"

static const char *TAG = "plan_tpl";

// ---------------------------------------------------------------------------
// System templates
//
// The app's own five system templates, taken 1:1 from a plan the vendor app
// wrote from them (captured 2026-10-08). Light blocks are exactly what the
// app sends: in PPFD mode the timePeriod carries only enabled/weekmask and
// the light is off with level 11; Drying is Schedule mode with everything
// disabled. Each template has its own colour, which together are the only
// five colours the controller knows (1-5).
// ---------------------------------------------------------------------------
typedef struct {
    const char *id;
    const char *name;
    int days, color;
    int on_s, off_s;       // PPFD window (and day window), s since midnight
    int ppfd;              // PPFD target; 0 = the Drying light block
    int day_s, night_s;    // target dayTime
    int t_d, t_n, t_db, h_d, h_n, h_db, c_d, c_n, c_db;
} sys_tpl_t;

static const sys_tpl_t SYS[PLAN_TPL_SYSTEM] = {
    { "sys:seedling", "Seedling",           14, 1, 18000, 82800, 300, 18000, 82800, 23, 23, 3, 70, 70,  5,  600, 400, 200 },
    { "sys:clone",    "Cloning & Seedling", 14, 2, 18000, 82800, 200, 18000, 82800, 26, 22, 2, 85, 85,  5,  600, 400, 200 },
    { "sys:veg",      "Vegetative Growth",  28, 3, 18000, 82800, 600, 18000, 82800, 26, 23, 3, 65, 65, 10,  600, 400, 200 },
    { "sys:flower",   "Flowering",          56, 4, 18000, 61200, 900, 18000, 61200, 25, 23, 3, 55, 55,  5, 1000, 400, 200 },
    { "sys:dry",      "Drying",             10, 5,     0,     0,   0, 18000, 61200, 18, 18, 3, 50, 50,  5,  400, 400, 200 },
};

const char *plan_tpl_system_id(int i)
{
    return (i >= 0 && i < PLAN_TPL_SYSTEM) ? SYS[i].id : NULL;
}

static const sys_tpl_t *sys_find(const char *id)
{
    for (int i = 0; i < PLAN_TPL_SYSTEM; i++)
        if (strcmp(SYS[i].id, id) == 0) return &SYS[i];
    return NULL;
}

static int light_text(char *o, size_t n, const sys_tpl_t *t)
{
    if (t->ppfd <= 0) {
        // Drying: Schedule mode, both periods disabled, light off.
        return snprintf(o, n,
            "{\"modeType\":1,\"darkTemp\":0.0,\"offTemp\":0.0,"
            "\"timePeriod\":[{\"enabled\":0,\"weekmask\":127}],"
            "\"ppfdPeriod\":[{\"enabled\":0,\"weekmask\":127,\"startTime\":0,\"endTime\":0,\"brightness\":20,\"fadeTime\":0}],"
            "\"mOnOff\":0,\"mLevel\":0,\"ppfdMinBrightness\":11,\"ppfdMaxBrightness\":100}");
    }
    return snprintf(o, n,
        "{\"modeType\":12,\"darkTemp\":0.0,\"offTemp\":0.0,"
        "\"timePeriod\":[{\"enabled\":1,\"weekmask\":127}],"
        "\"ppfdPeriod\":[{\"enabled\":1,\"weekmask\":127,\"startTime\":%d,\"endTime\":%d,\"brightness\":%d,\"fadeTime\":1800}],"
        "\"mOnOff\":0,\"mLevel\":11,\"ppfdMinBrightness\":11,\"ppfdMaxBrightness\":100}",
        t->on_s, t->off_s, t->ppfd);
}

static size_t sys_text(const sys_tpl_t *t, char *out, size_t n)
{
    char l[420];
    light_text(l, sizeof(l), t);
    int len = snprintf(out, n,
        "{\"name\":\"%s\",\"days\":%d,\"color\":%d,\"light1\":%s,\"light2\":%s,"
        "\"target\":{\"dayTime\":{\"startTime\":%d,\"endTime\":%d},"
        "\"temp\":{\"targetDay\":%d.0,\"targetNight\":%d.0,\"deadband\":%d.0},"
        "\"humi\":{\"targetDay\":%d.0,\"targetNight\":%d.0,\"deadband\":%d.0},"
        "\"co2\":{\"targetDay\":%d.0,\"targetNight\":%d.0,\"deadband\":%d.0}}}",
        t->name, t->days, t->color, l, l, t->day_s, t->night_s,
        t->t_d, t->t_n, t->t_db, t->h_d, t->h_n, t->h_db, t->c_d, t->c_n, t->c_db);
    return (len > 0 && (size_t)len < n) ? (size_t)len : 0;
}

// ---------------------------------------------------------------------------
// Custom templates in the storage partition
//
// Sector i at CUST_BASE + i * 4 KB: [magic][len][json]. The BLE job log
// uses the start of the partition (up to 0x7000).
// ---------------------------------------------------------------------------
#define CUST_BASE  0x10000
#define CUST_MAGIC 0x54504C31u   // "TPL1"

static const esp_partition_t *part(void)
{
    static const esp_partition_t *p = NULL;
    if (!p) p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    return p;
}

static int cust_slot(const char *id)
{
    if (strncmp(id, "cust:", 5) != 0) return -1;
    int i = atoi(id + 5);
    return (i >= 0 && i < PLAN_TPL_CUSTOM && id[5] >= '0' && id[5] <= '9') ? i : -1;
}

static size_t cust_read(int i, char *out, size_t n)
{
    const esp_partition_t *p = part();
    if (!p || n < 2) return 0;
    uint32_t hdr[2];
    size_t off = CUST_BASE + (size_t)i * 0x1000;
    if (off + 0x1000 > p->size || esp_partition_read(p, off, hdr, sizeof(hdr)) != ESP_OK) return 0;
    if (hdr[0] != CUST_MAGIC || hdr[1] == 0 || hdr[1] >= n || hdr[1] > 0x1000 - 8) return 0;
    if (esp_partition_read(p, off + 8, out, hdr[1]) != ESP_OK) return 0;
    out[hdr[1]] = '\0';
    return hdr[1];
}

static bool cust_used(int i)
{
    const esp_partition_t *p = part();
    uint32_t hdr[2];
    size_t off = CUST_BASE + (size_t)i * 0x1000;
    if (!p || off + 0x1000 > p->size || esp_partition_read(p, off, hdr, sizeof(hdr)) != ESP_OK) return false;
    return hdr[0] == CUST_MAGIC && hdr[1] > 0 && hdr[1] <= 0x1000 - 8;
}

static bool cust_write(int i, const char *json, size_t len)
{
    const esp_partition_t *p = part();
    if (!p) return false;
    size_t off = CUST_BASE + (size_t)i * 0x1000;
    if (off + 0x1000 > p->size || len > 0x1000 - 8) return false;
    if (esp_partition_erase_range(p, off, 0x1000) != ESP_OK) return false;
    if (!json) return true;   // erased = empty
    if (esp_partition_write(p, off + 8, json, len) != ESP_OK) return false;
    uint32_t hdr[2] = { CUST_MAGIC, (uint32_t)len };
    return esp_partition_write(p, off, hdr, sizeof(hdr)) == ESP_OK;
}

// ---------------------------------------------------------------------------

size_t plan_tpl_get(const char *id, char *out, size_t n)
{
    if (!id || !out || !n) return 0;
    const sys_tpl_t *t = sys_find(id);
    if (t) return sys_text(t, out, n);
    int i = cust_slot(id);
    return i >= 0 ? cust_read(i, out, n) : 0;
}

void plan_tpl_name(const char *id, char *out, size_t n)
{
    out[0] = '\0';
    char *buf = malloc(PLAN_TPL_MAX_LEN);
    if (!buf) return;
    size_t l = plan_tpl_get(id, buf, PLAN_TPL_MAX_LEN);
    const char *v;
    size_t vl;
    if (l && jtext_find(buf, l, "name", &v, &vl) && vl >= 2 && v[0] == '"') {
        size_t c = vl - 2 < n - 1 ? vl - 2 : n - 1;
        memcpy(out, v + 1, c);
        out[c] = '\0';
    }
    free(buf);
}

// Rebuilds a template object with only the known keys, so whatever the
// web page sends cannot store anything else. name replaces "name" if set.
static char *tpl_normalise(const char *json, const char *name)
{
    size_t jl = strlen(json);
    if (jl < 2 || json[0] != '{') return NULL;
    static const char *const KEYS[] = { "light1", "light2", "target" };
    const char *v[3];
    size_t vl[3];
    for (int k = 0; k < 3; k++) {
        if (!jtext_find(json, jl, KEYS[k], &v[k], &vl[k]) || v[k][0] != '{') return NULL;
    }
    long days = jtext_int(json, jl, "days", 14);
    long color = jtext_int(json, jl, "color", 0);
    if (days < 1 || days > 365) days = 14;
    // The controller knows only colours 1-5.
    if (color < 1 || color > 5) color = 1;

    char nm[40] = "";
    if (name && name[0]) {
        // JSON-escape the given name (quotes and backslashes).
        size_t o = 0;
        for (const char *c = name; *c && o < sizeof(nm) - 2; c++) {
            if (*c == '"' || *c == '\\') nm[o++] = '\\';
            if ((unsigned char)*c >= 0x20) nm[o++] = *c;
        }
        nm[o] = '\0';
    } else {
        const char *nv;
        size_t nvl;
        if (jtext_find(json, jl, "name", &nv, &nvl) && nvl >= 2 && nv[0] == '"') {
            size_t c = nvl - 2 < sizeof(nm) - 1 ? nvl - 2 : sizeof(nm) - 1;
            memcpy(nm, nv + 1, c);
            nm[c] = '\0';
        }
    }
    if (!nm[0]) strcpy(nm, "Custom");

    size_t cap = vl[0] + vl[1] + vl[2] + 160;
    char *out = malloc(cap);
    if (!out) return NULL;
    snprintf(out, cap, "{\"name\":\"%s\",\"days\":%ld,\"color\":%ld,\"light1\":%.*s,\"light2\":%.*s,\"target\":%.*s}",
             nm, days, color, (int)vl[0], v[0], (int)vl[1], v[1], (int)vl[2], v[2]);
    return out;
}

bool plan_tpl_save(const char *id, const char *name, const char *json)
{
    int i = cust_slot(id);
    if (i < 0 || !json) return false;
    char *t = tpl_normalise(json, name);
    if (!t) return false;
    size_t tl = strlen(t);
    bool ok = tl < 0x1000 - 8 && cust_write(i, t, tl);
    ESP_LOGI(TAG, "Template %s %s (%u bytes)", id, ok ? "saved" : "not saved", (unsigned)tl);
    free(t);
    return ok;
}

bool plan_tpl_delete(const char *id)
{
    int i = cust_slot(id);
    if (i < 0) return false;
    bool ok = cust_write(i, NULL, 0);
    ESP_LOGI(TAG, "Template %s %s", id, ok ? "deleted" : "not deleted");
    return ok;
}

bool plan_tpl_clone(const char *src, const char *dst, const char *name, char *new_id, size_t n)
{
    char *buf = malloc(PLAN_TPL_MAX_LEN);
    if (!buf) return false;
    bool ok = false;
    if (plan_tpl_get(src, buf, PLAN_TPL_MAX_LEN)) {
        char id[16] = "";
        if (dst && cust_slot(dst) >= 0) {
            strncpy(id, dst, sizeof(id) - 1);
        } else {
            for (int i = 0; i < PLAN_TPL_CUSTOM && !id[0]; i++)
                if (!cust_used(i)) snprintf(id, sizeof(id), "cust:%d", i);
        }
        if (id[0]) {
            char nm[40];
            if (name && name[0]) {
                strncpy(nm, name, sizeof(nm) - 1);
                nm[sizeof(nm) - 1] = '\0';
            } else {
                plan_tpl_name(src, nm, sizeof(nm) - 9);
                strcat(nm, " (Copy)");
            }
            ok = plan_tpl_save(id, nm, buf);
            if (ok && new_id) snprintf(new_id, n, "%s", id);
        }
    }
    free(buf);
    return ok;
}

char *plan_tpl_make_stage(const char *id, int start_date, int days_override)
{
    char *buf = malloc(PLAN_TPL_MAX_LEN);
    if (!buf) return NULL;
    size_t l = plan_tpl_get(id, buf, PLAN_TPL_MAX_LEN);
    char *out = NULL;
    if (l) {
        const char *v[3];
        size_t vl[3];
        static const char *const KEYS[] = { "light1", "light2", "target" };
        bool ok = true;
        for (int k = 0; k < 3 && ok; k++) ok = jtext_find(buf, l, KEYS[k], &v[k], &vl[k]);
        const char *nv;
        size_t nvl;
        if (ok && !(jtext_find(buf, l, "name", &nv, &nvl) && nv[0] == '"')) { nv = "\"Stage\""; nvl = 7; }
        if (ok) {
            long days = days_override > 0 ? days_override : jtext_int(buf, l, "days", 14);
            long color = jtext_int(buf, l, "color", 1);
            if (color < 1 || color > 5) color = 1;
            int end = sf_plan_date_add_days(start_date, (int)days - 1);
            size_t cap = vl[0] + vl[1] + vl[2] + nvl + 200;
            out = malloc(cap);
            if (out) {
                // Unique per stage: wall-clock seconds like the app, plus a
                // counter for stages made within the same second.
                static uint32_t seq = 0;
                time_t now = time(NULL);
                unsigned long sid = (unsigned long)(now > 1600000000 ? now : 1791000000) + (seq++ % 997);
                snprintf(out, cap,
                    "{\"stageId\":%lu,\"label\":%.*s,\"startDate\":%d,\"endDate\":%d,\"alarmDate\":0,"
                    "\"color\":%ld,\"light1\":%.*s,\"light2\":%.*s,\"target\":%.*s}",
                    sid,
                    (int)nvl, nv, start_date, end, color,
                    (int)vl[0], v[0], (int)vl[1], v[1], (int)vl[2], v[2]);
            }
        }
    }
    free(buf);
    return out;
}

bool plan_tpl_list(plan_tpl_sink_t sink, void *ctx)
{
    char *buf = malloc(PLAN_TPL_MAX_LEN);
    if (!buf) return false;
    bool ok = sink(ctx, "{\"system\":[", 11);
    for (int i = 0; i < PLAN_TPL_SYSTEM && ok; i++) {
        size_t l = sys_text(&SYS[i], buf, PLAN_TPL_MAX_LEN);
        char head[48];
        int hl = snprintf(head, sizeof(head), "%s{\"id\":\"%s\",\"tpl\":", i ? "," : "", SYS[i].id);
        ok = sink(ctx, head, hl) && sink(ctx, buf, l) && sink(ctx, "}", 1);
    }
    // Only the used custom slots; "max" and "free" (the first empty slot,
    // or null) let the page offer a new one without listing 50 nulls.
    if (ok) ok = sink(ctx, "],\"custom\":[", 12);
    int n = 0, first_free = -1;
    for (int i = 0; i < PLAN_TPL_CUSTOM && ok; i++) {
        if (!cust_used(i)) { if (first_free < 0) first_free = i; continue; }
        size_t l = cust_read(i, buf, PLAN_TPL_MAX_LEN);
        if (!l) { if (first_free < 0) first_free = i; continue; }
        char head[48];
        int hl = snprintf(head, sizeof(head), "%s{\"id\":\"cust:%d\",\"tpl\":", n++ ? "," : "", i);
        ok = sink(ctx, head, hl) && sink(ctx, buf, l) && sink(ctx, "}", 1);
    }
    if (ok) {
        char tail[64];
        int tl = first_free >= 0
            ? snprintf(tail, sizeof(tail), "],\"max\":%d,\"free\":\"cust:%d\"}", PLAN_TPL_CUSTOM, first_free)
            : snprintf(tail, sizeof(tail), "],\"max\":%d,\"free\":null}", PLAN_TPL_CUSTOM);
        ok = sink(ctx, tail, tl);
    }
    free(buf);
    return ok;
}
