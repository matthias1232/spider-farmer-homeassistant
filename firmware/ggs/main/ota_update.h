#pragma once
#include "esp_http_server.h"

// ============================================================================
// Firmware update over the web interface
//
// Upload a .bin built by `idf.py build` and the bridge writes it to the
// inactive OTA slot, verifies it, switches over and restarts by itself.
// No serial cable, no BOOT button, no power cycle.
//
// Safety properties that matter here:
//
//   * The running firmware is never overwritten. The image goes to the
//     other slot, so a failed or aborted upload leaves the device
//     bootable on what it is running now.
//   * The image header is checked before anything is written, so an
//     unrelated file cannot be flashed by accident.
//   * The switch happens only after the whole image is received and
//     validated. Losing Wi-Fi mid-upload aborts it and changes nothing.
//   * The restart is deferred briefly so the browser still gets its
//     response, instead of seeing a connection reset and leaving the
//     user unsure whether the update worked.
// ============================================================================

// POST /update — receives the image, writes it, then reboots on success.
esp_err_t ota_post_handler(httpd_req_t *req);

// GET /update — the upload form and current firmware details.
esp_err_t ota_page_handler(httpd_req_t *req);

// POST /reboot — restarts the device on request.
esp_err_t reboot_handler(httpd_req_t *req);

// ---------------------------------------------------------------------------
// Updates from a URL
//
// The configured address points at either a firmware .bin or a JSON
// manifest of the form:
//
//     {"version":"2.3.0","url":"http://host/spiderbridge.bin"}
//
// A manifest is preferable: the version can be compared before any
// firmware is downloaded, so an up-to-date bridge transfers a few
// hundred bytes instead of a megabyte.
// ---------------------------------------------------------------------------

typedef struct {
    bool checked;              // a check has completed since boot
    bool available;            // the remote version differs from the running one
    bool checking;             // a check or install is running right now
    char remote_version[32];
    char running_version[32];
    char last_error[96];       // empty when the last check succeeded
    char last_check[32];       // "never", or seconds since the check
} ota_remote_status_t;

void ota_remote_get_status(ota_remote_status_t *out);

// Checks the configured URL at boot, after the uplink comes up. When the
// remote version is newer and automatic installation is enabled, it is
// installed and the device restarts; otherwise the result is only
// reported in the web interface.
void ota_remote_start(void);

// POST /update/check  — check now, without installing.
esp_err_t ota_check_handler(httpd_req_t *req);

// POST /update/remote — install from the configured URL now.
esp_err_t ota_remote_handler(httpd_req_t *req);
