#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"

#include "sb_config.h"
#include "provisioning.h"
#include "time_sync.h"

static const char *TAG = "time";

static bool s_synced = false;
static char s_server[64] = "";

static void on_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);

    // %Z rather than tm_zone: this newlib build does not carry the
    // BSD extension fields in struct tm.
    char buf[32], zone[8];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    strftime(zone, sizeof(zone), "%Z", &lt);
    ESP_LOGI(TAG, "Clock synchronised: %s %s", buf, zone);
}

// Minutes east of UTC, worked out by comparing the same instant
// interpreted both ways. tm_gmtoff is not available here.
static int utc_offset_minutes(time_t when, const struct tm *local)
{
    struct tm utc;
    gmtime_r(&when, &utc);

    int lm = local->tm_hour * 60 + local->tm_min;
    int um = utc.tm_hour * 60 + utc.tm_min;
    int diff = lm - um;

    // Correct for the comparison straddling midnight.
    int dday = local->tm_yday - utc.tm_yday;
    if (dday == 1 || dday < -1) diff += 1440;
    else if (dday == -1 || dday > 1) diff -= 1440;

    return diff;
}

// Builds the TZ string actually applied.
//
// In automatic mode the stored rules are used unchanged, so the switch
// happens on the usual dates. Forcing standard or summer time means
// pinning the offset: the rule part is dropped, because a rule would
// override the choice at the next switch date.
static void build_tz(const sb_prov_cfg_t *cfg, char *out, size_t out_sz)
{
    if (cfg->dst_mode == SB_DST_AUTO || !cfg->tz[0]) {
        strncpy(out, cfg->tz[0] ? cfg->tz : SB_TZ_DEFAULT, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

    // Split "CET-1CEST,M3.5.0,M10.5.0/3" into the names and the rules.
    char base[48];
    strncpy(base, cfg->tz, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    char *comma = strchr(base, ',');
    if (comma) *comma = '\0';      // drop the switching rules

    if (cfg->dst_mode == SB_DST_STANDARD) {
        // Keep only the standard name and offset: find where the second
        // zone name begins and cut there.
        //
        // "CET-1CEST" -> "CET-1". The standard offset ends at the first
        // letter following the digits.
        size_t i = 0;
        while (base[i] && !(base[i] == '-' || base[i] == '+' ||
                            (base[i] >= '0' && base[i] <= '9'))) i++;
        while (base[i] == '-' || base[i] == '+') i++;
        while (base[i] >= '0' && base[i] <= '9') i++;
        if (base[i] == ':') { i++; while (base[i] >= '0' && base[i] <= '9') i++; }
        base[i] = '\0';
        strncpy(out, base, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

    // Forced daylight saving: name the summer zone and shift the offset
    // by one hour, with no rules so it never switches back.
    //
    // Parsed rather than hardcoded, so a zone whose saving is not one
    // hour still ends up with the right offset.
    char std_name[8] = "", dst_name[8] = "";
    int off_h = 0, off_m = 0;
    size_t i = 0, n = 0;
    while (base[i] && n < sizeof(std_name) - 1 &&
           !(base[i] == '-' || base[i] == '+' ||
             (base[i] >= '0' && base[i] <= '9'))) {
        std_name[n++] = base[i++];
    }
    std_name[n] = '\0';

    int sign = 1;
    if (base[i] == '-') { sign = -1; i++; }
    else if (base[i] == '+') { i++; }
    while (base[i] >= '0' && base[i] <= '9') off_h = off_h * 10 + (base[i++] - '0');
    if (base[i] == ':') {
        i++;
        while (base[i] >= '0' && base[i] <= '9') off_m = off_m * 10 + (base[i++] - '0');
    }

    n = 0;
    while (base[i] && n < sizeof(dst_name) - 1) dst_name[n++] = base[i++];
    dst_name[n] = '\0';

    // One hour further east while saving is in force; the TZ offset sign
    // is inverted relative to UTC, hence the subtraction.
    int total = sign * (off_h * 60 + off_m) - 60;
    int at = total < 0 ? -total : total;
    snprintf(out, out_sz, "%s%s%d:%02d",
             dst_name[0] ? dst_name : std_name,
             total < 0 ? "-" : "", at / 60, at % 60);
}

void time_sync_apply_tz(void)
{
    // On the heap, not the stack: this is called from app_main and from
    // HTTP handlers, and the struct is close to a kilobyte. Keeping it
    // on the stack is what overflowed the main task once the OTA and
    // clock fields were added.
    sb_prov_cfg_t *prov = malloc(sizeof(*prov));
    if (!prov) return;
    sb_prov_load(prov);

    char tz[64];
    build_tz(prov, tz, sizeof(tz));

    setenv("TZ", tz, 1);
    tzset();

    ESP_LOGI(TAG, "Time zone: %s%s", tz,
             prov->dst_mode == SB_DST_STANDARD ? " (standard time forced)"
           : prov->dst_mode == SB_DST_SUMMER   ? " (daylight saving forced)"
                                               : "");
    free(prov);
}

void time_sync_start(void)
{
    sb_prov_cfg_t *prov = malloc(sizeof(*prov));
    if (!prov) return;
    sb_prov_load(prov);

    // The zone applies immediately, with or without a network.
    time_sync_apply_tz();

    const char *server = prov->ntp_server[0] ? prov->ntp_server : SB_NTP_SERVER_DEFAULT;
    strncpy(s_server, server, sizeof(s_server) - 1);
    free(prov);

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_server);
    sntp_set_time_sync_notification_cb(on_sync);
    esp_sntp_init();

    ESP_LOGI(TAG, "NTP client started, server %s", s_server);
}

// ---------------------------------------------------------------------------
// Settable over MQTT and from the web interface
// ---------------------------------------------------------------------------

const char *time_sync_dst_label(void)
{
    sb_prov_cfg_t prov;
    sb_prov_load(&prov);
    switch (prov.dst_mode) {
        case SB_DST_STANDARD: return "Standard";
        case SB_DST_SUMMER:   return "Summer";
        default:              return "Automatic";
    }
}

// Rejects anything that is not usable as a TZ string, so a typo cannot
// leave the bridge on a clock nobody intended.
//
// tzset() silently falls back to UTC for malformed input, which would
// otherwise shift every schedule display by an hour with no warning.
static bool tz_is_plausible(const char *tz)
{
    if (!tz || !tz[0] || strlen(tz) > 47) return false;

    // At least three letters of zone name, then a digit somewhere.
    int letters = 0;
    for (int i = 0; i < 3; i++) {
        char c = tz[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) letters++;
    }
    if (letters < 3) return false;

    for (const char *p = tz; *p; p++) {
        if (*p >= '0' && *p <= '9') return true;
    }
    // A name with no offset, e.g. "UTC", is valid too.
    return strlen(tz) <= 5;
}

bool time_sync_set_timezone(const char *tz)
{
    if (!tz_is_plausible(tz)) {
        ESP_LOGW(TAG, "Rejected time zone '%s'", tz ? tz : "");
        return false;
    }

    sb_prov_cfg_t prov;
    sb_prov_load(&prov);
    strncpy(prov.tz, tz, sizeof(prov.tz) - 1);
    prov.tz[sizeof(prov.tz) - 1] = '\0';
    sb_prov_save(&prov);

    time_sync_apply_tz();
    ESP_LOGI(TAG, "Time zone set to %s", tz);
    return true;
}

bool time_sync_set_dst_mode(const char *mode_label)
{
    if (!mode_label) return false;

    int mode;
    if (strcasecmp(mode_label, "Automatic") == 0 ||
        strcasecmp(mode_label, "auto") == 0) {
        mode = SB_DST_AUTO;
    } else if (strcasecmp(mode_label, "Standard") == 0 ||
               strcasecmp(mode_label, "winter") == 0) {
        mode = SB_DST_STANDARD;
    } else if (strcasecmp(mode_label, "Summer") == 0 ||
               strcasecmp(mode_label, "dst") == 0) {
        mode = SB_DST_SUMMER;
    } else {
        ESP_LOGW(TAG, "Unknown daylight saving mode '%s'", mode_label);
        return false;
    }

    sb_prov_cfg_t prov;
    sb_prov_load(&prov);
    prov.dst_mode = mode;
    sb_prov_save(&prov);

    time_sync_apply_tz();
    return true;
}

bool time_sync_set_ntp_server(const char *server)
{
    if (!server || !server[0] || strlen(server) > 63) return false;

    sb_prov_cfg_t prov;
    sb_prov_load(&prov);
    strncpy(prov.ntp_server, server, sizeof(prov.ntp_server) - 1);
    prov.ntp_server[sizeof(prov.ntp_server) - 1] = '\0';
    sb_prov_save(&prov);

    strncpy(s_server, server, sizeof(s_server) - 1);

    // Restart the client so the new server is used right away rather
    // than at the next poll interval.
    esp_sntp_stop();
    esp_sntp_setservername(0, s_server);
    esp_sntp_init();

    ESP_LOGI(TAG, "NTP server set to %s", s_server);
    return true;
}

void time_sync_get_status(time_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->synced = s_synced;
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    strncpy(out->server, s_server, sizeof(out->server) - 1);

    const char *tz = getenv("TZ");
    if (tz) strncpy(out->zone, tz, sizeof(out->zone) - 1);

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);

    strftime(out->now, sizeof(out->now), "%Y-%m-%d %H:%M:%S", &lt);
    strftime(out->abbrev, sizeof(out->abbrev), "%Z", &lt);
    out->is_dst = lt.tm_isdst > 0;
    out->utc_offset_min = utc_offset_minutes(now, &lt);
}
