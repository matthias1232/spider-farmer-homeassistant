#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "web_auth.h"
#include "nav.h"
#include "about_page.h"

static const char ABOUT_PAGE[] =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>SpiderBridge About</title><style>"
"*{box-sizing:border-box}"
"body{font-family:system-ui,sans-serif;max-width:46rem;margin:0 auto;"
"padding:1rem;background:#13151a;color:#e6e6e6}"
"h1{font-size:1.3rem;margin:0 0 .2rem}"
"p.sub{margin:0 0 1rem;color:#9aa0aa;font-size:.85rem}"
"a{color:#8ab4f8;text-decoration:none}"
"a:hover{text-decoration:underline}"
"fieldset{border:1px solid #2c303a;border-radius:.5rem;padding:.9rem;"
"margin:0 0 1rem;background:#1a1d24}"
"legend{color:#8ab4f8;font-size:.9rem;padding:0 .4rem}"
".row{display:flex;gap:.6rem;padding:.35rem 0;font-size:.85rem;"
"border-bottom:1px solid #22262e;flex-wrap:wrap;align-items:center}"
".row:last-child{border-bottom:0}"
".lbl{flex:1;min-width:10rem;color:#9aa0aa}"
".v{font-family:ui-monospace,monospace;font-size:.82rem}"
".btn-bmc{display:inline-block;background:#fbbf24;color:#13151a;"
"font-weight:600;padding:.4rem .85rem;border-radius:.35rem;"
"text-decoration:none;font-size:.85rem;margin:.3rem 0}"
".btn-bmc:hover{background:#f59e0b;text-decoration:none}"
".btn-gh{display:inline-block;background:#24292f;color:#e6e6e6;"
"border:1px solid #3c414d;font-weight:500;padding:.4rem .85rem;"
"border-radius:.35rem;text-decoration:none;font-size:.85rem;margin:.3rem 0}"
".btn-gh:hover{background:#2f353f;text-decoration:none}"
".desc{font-size:.85rem;color:#c3c8d0;line-height:1.5;margin:0 0 .7rem}";

static const char ABOUT_BODY_OPEN[] =
"</style></head><body>";

static const char ABOUT_PAGE_BODY[] =
"<h1>About SpiderBridge</h1>"
"<p class=\"sub\">Local Home Assistant MQTT bridge for Spider Farmer GGS controllers</p>"
"<fieldset><legend>Project &amp; Source Code</legend>"
"<div class=\"row\"><span class=\"lbl\">GitHub Repository</span>"
"<span><a class=\"btn-gh\" href=\"https://github.com/matthias1232/spider-farmer-homeassistant\" target=\"_blank\" rel=\"noopener\">spider-farmer-homeassistant</a></span></div>"
"<div class=\"row\"><span class=\"lbl\">Documentation</span>"
"<span><a href=\"https://matthias1232.github.io/spider-farmer-homeassistant/\" target=\"_blank\" rel=\"noopener\">Online Documentation &amp; Setup Guide</a></span></div>"
"<div class=\"row\"><span class=\"lbl\">Web Installer</span>"
"<span><a href=\"https://matthias1232.github.io/spider-farmer-homeassistant/installer/\" target=\"_blank\" rel=\"noopener\">Browser Firmware Flasher</a></span></div>"
"<div class=\"row\"><span class=\"lbl\">Author</span><span>matthias1232</span></div>"
"<div class=\"row\"><span class=\"lbl\">License</span><span>GPL-3.0-or-later</span></div>"
"</fieldset>"
"<fieldset><legend>Support &amp; Donations</legend>"
"<p class=\"desc\">SpiderBridge is free, open-source software. If the project helps you and you would like to support ongoing development and maintenance, you can buy me a coffee:</p>"
"<div class=\"row\"><span class=\"lbl\">Buy Me a Coffee</span>"
"<span><a class=\"btn-bmc\" href=\"https://www.buymeacoffee.com/matthias1232\" target=\"_blank\" rel=\"noopener\">&#9749; buymeacoffee.com/matthias1232</a></span></div>"
"</fieldset>"
"<fieldset><legend>Firmware Information</legend>"
"<div class=\"row\"><span class=\"lbl\">Version</span><span class=\"v\" id=\"fw-ver\">...</span></div>"
"<div class=\"row\"><span class=\"lbl\">Project</span><span id=\"fw-proj\">spiderbridge_esp32_v2</span></div>"
"<div class=\"row\"><span class=\"lbl\">Built</span><span id=\"fw-built\">...</span></div>"
"</fieldset>"
"<script>"
"fetch('/status/data').then(r=>r.json()).then(d=>{"
"const f=d.fw||d.firmware||{};"
"if(f.version)document.getElementById('fw-ver').textContent=f.version;"
"if(f.built)document.getElementById('fw-built').textContent=f.built;"
"}).catch(()=>{});"
"</script></body></html>";

esp_err_t about_page_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, ABOUT_PAGE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NAV_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, ABOUT_BODY_OPEN, HTTPD_RESP_USE_STRLEN);
    nav_send(req, NAV_ABOUT);
    httpd_resp_send_chunk(req, ABOUT_PAGE_BODY, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}
