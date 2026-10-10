#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "supervisor.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sb_config.h"
#include "provisioning.h"
#include "wifi_apsta.h"
#include "web_auth.h"
#include "ota_update.h"
#include "nav.h"
#include "device_registry.h"

static const char *TAG = "ota";

// Deferred restart.
//
// Rebooting inside the request handler would cut the TCP connection
// before the response is flushed, so the browser shows a network error
// after a successful update — indistinguishable from a failed one.
static void restart_task(void *arg)
{
    int delay_ms = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    device_registry_flush();
    ESP_LOGI(TAG, "Restarting now");
    sv_restart(SV_WHY_USER);
}

static void restart_in(int delay_ms)
{
    xTaskCreate(restart_task, "sb_restart", 2048,
                (void *)(intptr_t)delay_ms, 5, NULL);
}

esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty upload");
        return ESP_FAIL;
    }

    // Always the slot that is not running, so a failure leaves the
    // current firmware intact and bootable.
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition available");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Update started: %d bytes into '%s'",
             req->content_len, target->label);

    // Buffer first, before esp_ota_begin claims its own memory.
    //
    // Sized small on purpose: while two TLS sessions are up, free
    // internal heap drops to around 41 KB. A 4 KB request on top of what
    // esp_ota_begin holds failed there, and the upload came back as a
    // bare HTTP 500 with nothing explaining it. 1 KB costs a few more
    // write calls and always fits.
    const size_t CHUNK = 1024;
    char *buf = malloc(CHUNK);
    if (!buf) {
        ESP_LOGE(TAG, "Not enough memory for the upload buffer (%u bytes free)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Too little memory right now — try again in a "
                             "moment", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "esp_ota_begin failed: %s (%u bytes free)",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Could not start the update — not enough memory "
                             "right now", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int received = 0;
    bool header_checked = false;
    bool failed = false;
    const char *why = "Upload failed";

    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf, CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            // A dropped connection mid-upload: abort and keep running on
            // the existing firmware.
            why = "Connection lost during upload";
            failed = true;
            break;
        }

        // Verify the image looks like firmware for this chip before
        // writing anything, so picking the wrong file in the browser
        // cannot brick the spare slot.
        //
        // The descriptor spans 288 bytes, which fits one chunk — but a
        // first read may still return less, so a short read is not
        // treated as a bad file. Only the magic number decides that.
        if (!header_checked) {
            const int NEED = (int)(sizeof(esp_image_header_t) +
                                   sizeof(esp_image_segment_header_t) +
                                   sizeof(esp_app_desc_t));
            if (r < NEED) {
                if (received + r >= req->content_len) {
                    why = "File is too small to be firmware";
                    failed = true;
                    break;
                }
                // Write what arrived and check the header next time.
                if (esp_ota_write(handle, buf, r) != ESP_OK) {
                    why = "Writing to flash failed";
                    failed = true;
                    break;
                }
                received += r;
                continue;
            }
            esp_image_header_t *hdr = (esp_image_header_t *)buf;
            if (hdr->magic != ESP_IMAGE_HEADER_MAGIC) {
                why = "That file is not an ESP32 firmware image";
                failed = true;
                break;
            }

            esp_app_desc_t *desc = (esp_app_desc_t *)(buf +
                sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
            ESP_LOGI(TAG, "Incoming firmware: %s %s", desc->project_name,
                     desc->version);
            header_checked = true;
        }

        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            why = "Writing to flash failed";
            failed = true;
            break;
        }
        received += r;
    }

    free(buf);

    if (failed) {
        esp_ota_abort(handle);
        ESP_LOGW(TAG, "Update aborted after %d bytes — still running the "
                      "current firmware", received);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, why, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Validates the checksum of the whole image. Only a complete, intact
    // image gets this far.
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req,
                        err == ESP_ERR_OTA_VALIDATE_FAILED
                            ? "The image failed validation and was discarded"
                            : "Finalising the update failed",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not switch to the new firmware");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Update complete (%d bytes). Restarting into '%s'.",
             received, target->label);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "ok", 2);

    // Long enough for the response to reach the browser.
    restart_in(1200);
    return ESP_OK;
}

esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    ESP_LOGI(TAG, "Restart requested from the web interface");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "ok", 2);
    restart_in(800);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /update
// ---------------------------------------------------------------------------
static const char OTA_PAGE_HEAD[] =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>SpiderBridge Firmware</title><style>"
"*{box-sizing:border-box}"
"body{font-family:system-ui,sans-serif;max-width:40rem;margin:0 auto;"
"padding:1rem;background:#13151a;color:#e6e6e6}"
"h1{font-size:1.3rem;margin:0 0 .2rem}"
"p.sub{margin:0 0 1rem;color:#9aa0aa;font-size:.85rem}"
"a{color:#8ab4f8;text-decoration:none}"
"fieldset{border:1px solid #2c303a;border-radius:.5rem;padding:.9rem;"
"margin:0 0 1rem;background:#1a1d24}"
"legend{color:#8ab4f8;font-size:.9rem;padding:0 .4rem}"
".row{display:flex;gap:.6rem;padding:.35rem 0;font-size:.85rem;"
"border-bottom:1px solid #22262e;flex-wrap:wrap}"
".row:last-child{border-bottom:0}"
".lbl{flex:1;min-width:9rem;color:#9aa0aa}"
"button{padding:.45rem 1rem;border:1px solid #333845;border-radius:.3rem;"
"background:#1a1d24;color:#c3c8d0;cursor:pointer;font-size:.85rem}"
"button.go{background:#8ab4f8;color:#13151a;border-color:#8ab4f8;font-weight:600}"
"button.danger{border-color:#7f1d1d;color:#fca5a5}"
"input[type=file]{font-size:.82rem;color:#c3c8d0;width:100%}"
"#bar{height:.5rem;background:#2c303a;border-radius:.25rem;overflow:hidden;"
"margin:.6rem 0;display:none}"
"#fill{height:100%;width:0;background:#8ab4f8;transition:width .2s}"
"#st{font-size:.85rem;color:#9aa0aa;min-height:1.2rem}";

// Split so the shared nav bar's CSS can be inserted while the <style>
// block is still open, and its markup right after <body> — matching
// every other page.
static const char OTA_PAGE_BODY_OPEN[] =
"</style></head><body>"
"<h1>Firmware</h1>";

static const char OTA_PAGE_TAIL[] =
// Update from the configured URL.
//
// Both routes already existed but nothing called them, so the whole
// URL-update path was unreachable from the interface.
"<fieldset><legend>Update from the configured address</legend>"
"<p class=\"sub\" id=\"ustat\">&nbsp;</p>"
"<p><button id=\"bchk\" onclick=\"chk()\">Check now</button> "
"<button id=\"bpull\" onclick=\"pull()\">Install from URL</button></p>"
"<small>The address is set under Settings &rarr; Firmware updates. "
"Checking only reports what is available; installing downloads it and "
"restarts.</small>"
"</fieldset>"
"<fieldset><legend>Install an update</legend>"
"<p class=\"sub\">Pick the <code>.bin</code> from <code>build/esp32/</code>. "
"The image is written to the spare slot and only activated once it has been "
"verified, so an interrupted upload leaves the current firmware running. "
"The device restarts on its own when it is done.</p>"
"<input type=\"file\" id=\"f\" accept=\".bin\">"
"<div id=\"bar\"><div id=\"fill\"></div></div>"
"<div id=\"st\"></div>"
"<p><button class=\"go\" id=\"go\" onclick=\"up()\">Upload and restart</button> "
"<button class=\"danger\" onclick=\"rb()\">Restart only</button></p>"
"</fieldset>"
"<script>"
"function st(t){document.getElementById('st').textContent=t;}"
"function up(){"
"const f=document.getElementById('f').files[0];"
"if(!f){st('Choose a firmware file first.');return;}"
"document.getElementById('go').disabled=true;"
"document.getElementById('bar').style.display='block';"
"const x=new XMLHttpRequest();"
"x.upload.onprogress=e=>{if(e.lengthComputable){"
"const p=Math.round(e.loaded/e.total*100);"
"document.getElementById('fill').style.width=p+'%';"
"st('Uploading '+p+'%');}};"
"x.onload=()=>{"
"if(x.status===200){st('Installed. Restarting — this page comes back in "
"about 15 seconds.');setTimeout(()=>location.href='/status',15000);}"
"else{st('Failed: '+x.responseText);"
"document.getElementById('go').disabled=false;}};"
"x.onerror=()=>{st('Connection lost during the upload. Nothing was changed.');"
"document.getElementById('go').disabled=false;};"
"x.open('POST','/update');x.send(f);}"
"function rb(){"
"if(!confirm('Restart the bridge now?'))return;"
"fetch('/reboot',{method:'POST'}).then(()=>{"
"st('Restarting — back in about 15 seconds.');"
"setTimeout(()=>location.href='/status',15000);});}"
// --- URL updates ---
"function ust(t){document.getElementById('ustat').textContent=t;}"
"async function chk(){"
"ust('Checking...');"
"try{const r=await fetch('/update/check',{method:'POST'});"
"ust(await r.text());setTimeout(poll,2500);}"
"catch(e){ust('Could not start the check.');}}"
"async function poll(){"
// The check runs on its own task, so the result arrives via /status/data.
"try{const r=await fetch('/status/data');const j=await r.json();"
"const f=j.fw||{};"
"if(f.ota_error){ust('Check failed: '+f.ota_error);}"
"else if(f.ota_available){ust('Update available: '+"
"(f.ota_remote||'unversioned')+' (running '+f.version+')');}"
"else if(f.ota_checked){ust('Up to date (running '+f.version+')');}"
"else{setTimeout(poll,2500);}}"
"catch(e){}}"
"async function pull(){"
"if(!confirm('Download and install from the configured address?'))return;"
"ust('Downloading...');"
"try{const r=await fetch('/update/remote',{method:'POST'});"
"ust(await r.text()+' The page returns once it restarts.');"
"setTimeout(()=>location.href='/status',30000);}"
"catch(e){ust('Could not start the update.');}}"
"poll();"
"</script></body></html>";

// ---------------------------------------------------------------------------
// Updates from a URL
// ---------------------------------------------------------------------------

static ota_remote_status_t s_remote = {0};
static int64_t s_last_check_us = 0;

void ota_remote_get_status(ota_remote_status_t *out)
{
    if (!out) return;
    *out = s_remote;

    const esp_app_desc_t *app = esp_app_get_description();
    strncpy(out->running_version, app->version, sizeof(out->running_version) - 1);

    if (s_last_check_us) {
        uint32_t ago = (uint32_t)((esp_timer_get_time() - s_last_check_us) / 1000000);
        if (ago < 90) {
            snprintf(out->last_check, sizeof(out->last_check), "%us ago",
                     (unsigned)ago);
        } else if (ago < 5400) {
            snprintf(out->last_check, sizeof(out->last_check), "%um ago",
                     (unsigned)(ago / 60));
        } else {
            snprintf(out->last_check, sizeof(out->last_check), "%uh ago",
                     (unsigned)(ago / 3600));
        }
    } else {
        strncpy(out->last_check, "never", sizeof(out->last_check) - 1);
    }
}

// Compares dotted version strings numerically, so 2.10.0 ranks above
// 2.9.0 — a plain string comparison gets that backwards.
static int version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        int na = 0, nb = 0;
        while (*a >= '0' && *a <= '9') na = na * 10 + (*a++ - '0');
        while (*b >= '0' && *b <= '9') nb = nb * 10 + (*b++ - '0');
        if (na != nb) return na < nb ? -1 : 1;
        // Skip one separator on each side.
        if (*a) a++;
        if (*b) b++;
    }
    return 0;
}

// Fetches the manifest and records what it advertises. Returns the URL
// of the firmware image in img_url when one is given.
//
// A URL ending in .bin is taken as the image itself, with no version
// information available until it is downloaded.
static bool fetch_manifest(const char *url, char *version, size_t version_sz,
                           char *img_url, size_t img_url_sz, char *err, size_t err_sz)
{
    size_t len = strlen(url);
    if (len > 4 && strcasecmp(url + len - 4, ".bin") == 0) {
        version[0] = '\0';
        strncpy(img_url, url, img_url_sz - 1);
        img_url[img_url_sz - 1] = '\0';
        return true;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = NULL,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) {
        snprintf(err, err_sz, "Could not start the HTTP client");
        return false;
    }

    bool ok = false;
    char body[512] = {0};

    esp_err_t e = esp_http_client_open(cl, 0);
    if (e != ESP_OK) {
        snprintf(err, err_sz, "Cannot reach %.60s", url);
    } else {
        esp_http_client_fetch_headers(cl);
        int status = esp_http_client_get_status_code(cl);
        int n = esp_http_client_read(cl, body, sizeof(body) - 1);
        if (status != 200) {
            snprintf(err, err_sz, "Server replied %d", status);
        } else if (n <= 0) {
            snprintf(err, err_sz, "Empty reply from the server");
        } else {
            body[n] = '\0';
            cJSON *root = cJSON_Parse(body);
            if (!root) {
                snprintf(err, err_sz, "Reply is not valid JSON");
            } else {
                cJSON *v = cJSON_GetObjectItem(root, "version");
                cJSON *u = cJSON_GetObjectItem(root, "url");
                if (cJSON_IsString(v) && cJSON_IsString(u)) {
                    strncpy(version, v->valuestring, version_sz - 1);
                    version[version_sz - 1] = '\0';
                    strncpy(img_url, u->valuestring, img_url_sz - 1);
                    img_url[img_url_sz - 1] = '\0';
                    ok = true;
                } else {
                    snprintf(err, err_sz, "Manifest needs 'version' and 'url'");
                }
                cJSON_Delete(root);
            }
        }
        esp_http_client_close(cl);
    }

    esp_http_client_cleanup(cl);
    return ok;
}

// Downloads and installs from a URL. On success the device restarts.
static bool install_from_url(const char *img_url, char *err, size_t err_sz)
{
    ESP_LOGI(TAG, "Downloading firmware from %s", img_url);

    esp_http_client_config_t http_cfg = {
        .url = img_url,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t e = esp_https_ota(&ota_cfg);
    if (e != ESP_OK) {
        snprintf(err, err_sz, "Download or install failed: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "%s", err);
        return false;
    }

    ESP_LOGI(TAG, "Installed from URL, restarting");
    return true;
}

// One check, optionally installing. Runs on its own task because it
// blocks on the network.
static void check_task(void *arg)
{
    bool allow_install = (bool)(intptr_t)arg;

    sb_prov_cfg_t prov;
    sb_prov_load(&prov);

    s_remote.checking = true;
    s_remote.last_error[0] = '\0';

    if (!prov.ota_url[0]) {
        strncpy(s_remote.last_error, "No update URL configured",
                sizeof(s_remote.last_error) - 1);
        s_remote.checking = false;
        vTaskDelete(NULL);
        return;
    }

    char version[32] = "", img_url[192] = "", err[96] = "";
    bool got = fetch_manifest(prov.ota_url, version, sizeof(version),
                              img_url, sizeof(img_url), err, sizeof(err));

    s_last_check_us = esp_timer_get_time();
    s_remote.checked = true;

    if (!got) {
        strncpy(s_remote.last_error, err, sizeof(s_remote.last_error) - 1);
        s_remote.available = false;
        s_remote.checking = false;
        ESP_LOGW(TAG, "Update check failed: %s", err);
        vTaskDelete(NULL);
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    strncpy(s_remote.remote_version, version, sizeof(s_remote.remote_version) - 1);

    // With a bare .bin there is no version to compare, so it counts as
    // available and the user decides.
    bool newer = !version[0] || version_cmp(app->version, version) < 0;
    s_remote.available = newer;

    if (!newer) {
        ESP_LOGI(TAG, "Up to date (running %s, offered %s)", app->version, version);
        s_remote.checking = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Update available: %s (running %s)",
             version[0] ? version : "unversioned image", app->version);

    if (allow_install) {
        if (install_from_url(img_url, err, sizeof(err))) {
            s_remote.checking = false;
            restart_in(1000);
            vTaskDelete(NULL);
            return;
        }
        strncpy(s_remote.last_error, err, sizeof(s_remote.last_error) - 1);
    } else {
        ESP_LOGI(TAG, "Automatic installation is off — install it from the "
                      "Firmware page when you are ready");
    }

    s_remote.checking = false;
    vTaskDelete(NULL);
}

// Boot-time check, once the uplink is up.
static void boot_check_task(void *arg)
{
    sb_prov_cfg_t prov;
    sb_prov_load(&prov);

    if (!prov.ota_check || !prov.ota_url[0]) {
        vTaskDelete(NULL);
        return;
    }

    // No point checking before there is a route to the internet.
    if (!wifi_apsta_wait_uplink(SB_OTA_BOOT_DELAY_S * 1000)) {
        ESP_LOGW(TAG, "No uplink at boot — skipping the update check");
        strncpy(s_remote.last_error, "No uplink when the check ran",
                sizeof(s_remote.last_error) - 1);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Checking %s for a newer firmware", prov.ota_url);
    check_task((void *)(intptr_t)prov.ota_auto);
    vTaskDelete(NULL);
}

void ota_remote_start(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    strncpy(s_remote.running_version, app->version,
            sizeof(s_remote.running_version) - 1);
    strncpy(s_remote.last_check, "never", sizeof(s_remote.last_check) - 1);

    xTaskCreate(boot_check_task, "ota_boot", 8192, NULL, 4, NULL);
}

esp_err_t ota_check_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    if (s_remote.checking) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "A check is already running",
                               HTTPD_RESP_USE_STRLEN);
    }

    xTaskCreate(check_task, "ota_check", 8192, (void *)(intptr_t)false, 4, NULL);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", 2);
}

esp_err_t ota_remote_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    if (s_remote.checking) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "An update is already running",
                               HTTPD_RESP_USE_STRLEN);
    }

    // true: download and install, then restart.
    xTaskCreate(check_task, "ota_pull", 8192, (void *)(intptr_t)true, 4, NULL);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", 2);
}

esp_err_t ota_page_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, OTA_PAGE_HEAD);
    httpd_resp_send_chunk(req, NAV_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_sendstr_chunk(req, OTA_PAGE_BODY_OPEN);
    nav_send(req, NAV_FIRMWARE);

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    // Generous: every field is variable-length and the compiler checks
    // the worst case, not the typical one.
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "<fieldset><legend>Running firmware</legend>"
             "<div class=row><span class=lbl>Version</span><span>%s</span></div>"
             "<div class=row><span class=lbl>Project</span><span>%s</span></div>"
             "<div class=row><span class=lbl>Built</span><span>%s %s</span></div>"
             "<div class=row><span class=lbl>IDF</span><span>%s</span></div>"
             "<div class=row><span class=lbl>Active slot</span><span>%s</span></div>"
             "<div class=row><span class=lbl>Update goes to</span><span>%s</span></div>"
             "</fieldset>",
             app->version, app->project_name, app->date, app->time,
             app->idf_ver,
             running ? running->label : "?",
             next ? next->label : "none");
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(req, OTA_PAGE_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}
