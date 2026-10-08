#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

#include "sb_config.h"
#include "provisioning.h"
#include "ip_filter.h"
#include "web_auth.h"

static const char *TAG = "web_auth";

static char s_password[33] = "";

void web_auth_reload(void)
{
    sb_prov_cfg_t prov;
    sb_prov_load(&prov);
    strncpy(s_password, prov.admin_pass, sizeof(s_password) - 1);
    s_password[sizeof(s_password) - 1] = '\0';

    ESP_LOGI(TAG, "Web interface %s",
             s_password[0] ? "requires a password" : "is open (no password set)");
}

bool web_auth_enabled(void)
{
    return s_password[0] != '\0';
}

// Constant-time comparison, so a wrong password cannot be guessed one
// character at a time by measuring how long the check takes.
static bool secure_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la ^ lb);
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

static void send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"SpiderBridge\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<html><body style=\"font-family:system-ui;background:#13151a;"
        "color:#e6e6e6;padding:2rem\">"
        "<h1>Login required</h1>"
        "<p>User <b>admin</b> and the password set in the bridge settings.</p>"
        "</body></html>", HTTPD_RESP_USE_STRLEN);
}

// --- Diagnostics and the heavy-request guard ---
//
// The HTTP server runs every handler on one task. Two heavy JSON requests
// (two tabs, or a page and the status page) used to queue behind each
// other and each held a large tree on the heap at the same time; the page
// then looked stuck. Heavy handlers take this flag: while one runs, a
// second gets 503 at once and the page retries, instead of waiting or
// doubling the heap peak.
static volatile uint32_t s_shed = 0, s_busy = 0, s_served = 0;
static volatile bool s_heavy = false;
static portMUX_TYPE s_heavy_mux = portMUX_INITIALIZER_UNLOCKED;

bool web_heavy_begin(httpd_req_t *req)
{
    bool got = false;
    portENTER_CRITICAL(&s_heavy_mux);
    if (!s_heavy) { s_heavy = true; got = true; }
    portEXIT_CRITICAL(&s_heavy_mux);
    if (!got) {
        s_busy++;
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "2");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "busy");
    }
    return got;
}

void web_heavy_end(void)
{
    portENTER_CRITICAL(&s_heavy_mux);
    s_heavy = false;
    portEXIT_CRITICAL(&s_heavy_mux);
}

void web_stats(uint32_t *served, uint32_t *shed, uint32_t *busy)
{
    if (served) *served = s_served;
    if (shed) *shed = s_shed;
    if (busy) *busy = s_busy;
}

bool web_auth_check(httpd_req_t *req)
{
    s_served++;
    // Polling clients reuse a connection for nothing: closing it after the
    // response frees the socket at once, so a second tab or the log viewer
    // never waits for an LRU purge (seen as a hanging page).
    httpd_resp_set_hdr(req, "Connection", "close");
    // Shed the request rather than run out of memory serving it.
    //
    // Every page handler allocates while rendering, and several build
    // JSON trees that are briefly larger than the response itself. When
    // free heap is already low, taking on more work is how a tight
    // moment becomes a failed allocation and then a crash — which is
    // what used to happen here, reported as "Out of memory" on every
    // request until the device rebooted.
    //
    // Refusing early with 503 keeps the device alive and tells the
    // client plainly to come back, instead of half-rendering a page and
    // dying partway through. The TLS proxy has the same kind of guard
    // (SB_MIN_HEAP_FOR_SESSION); this is the web server's equivalent.
    //
    // The threshold is deliberately well below what a normal page needs,
    // so this only trips in genuine trouble and never during ordinary
    // use — measured free heap in normal operation is 70 KB and up.
    size_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (heap < SB_MIN_HEAP_FOR_WEB) {
        s_shed++;
        ESP_LOGW(TAG, "Refusing a request: only %u bytes of heap left",
                 (unsigned)heap);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "5");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_send(req,
            "The bridge is low on memory and is shedding requests to stay "
            "up. Try again in a few seconds.", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    // Address rules first: a blocked client should not even be told
    // whether a password is required.
    //
    // Enforced here rather than per route because every handler already
    // calls this, so no route can be forgotten.
    if (!ip_filter_allows_request(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_send(req, "Not permitted from this address.",
                        HTTPD_RESP_USE_STRLEN);
        return false;
    }

    if (!s_password[0]) return true;   // no password configured

    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len > 200) {
        send_unauthorized(req);
        return false;
    }

    char header[216];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header,
                                    sizeof(header)) != ESP_OK) {
        send_unauthorized(req);
        return false;
    }

    if (strncasecmp(header, "Basic ", 6) != 0) {
        send_unauthorized(req);
        return false;
    }

    unsigned char decoded[160];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                              (const unsigned char *)header + 6,
                              strlen(header + 6)) != 0) {
        send_unauthorized(req);
        return false;
    }
    decoded[decoded_len] = '\0';

    // Decoded form is "user:password".
    char *colon = strchr((char *)decoded, ':');
    if (!colon) {
        send_unauthorized(req);
        return false;
    }
    *colon = '\0';
    const char *user = (const char *)decoded;
    const char *pass = colon + 1;

    if (strcmp(user, "admin") != 0 || !secure_equal(pass, s_password)) {
        ESP_LOGW(TAG, "Rejected a login attempt");
        send_unauthorized(req);
        return false;
    }

    return true;
}
