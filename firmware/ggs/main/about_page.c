#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "web_auth.h"
#include "nav.h"
#include "about_page.h"

static const char ABOUT_STYLE[] =
"*{box-sizing:border-box}"
"body{font-family:system-ui,sans-serif;max-width:42rem;margin:0 auto;"
"padding:1rem;background:#13151a;color:#e6e6e6}"
"h1{font-size:1.3rem;margin:0 0 .2rem}"
".sub{margin:0 0 1rem;color:#9aa0aa;font-size:.85rem}"
"a{color:#8ab4f8;text-decoration:none}"
"fieldset{border:1px solid #2c303a;border-radius:.5rem;padding:.8rem;"
"margin:0 0 .9rem;background:#1a1d24}"
"legend{color:#8ab4f8;font-size:.9rem;padding:0 .4rem}"
".row{display:flex;gap:.6rem;padding:.3rem 0;font-size:.85rem;"
"border-bottom:1px solid #22262e;flex-wrap:wrap;align-items:center}"
".row:last-child{border-bottom:0}"
".lbl{flex:1;min-width:9rem;color:#9aa0aa}"
".v{font-family:ui-monospace,monospace;font-size:.82rem}"
".b{display:inline-block;padding:.35rem .8rem;border-radius:.35rem;"
"font-size:.85rem;margin:.15rem 0}"
".bmc{background:#fbbf24;color:#13151a;font-weight:600}"
".gh{background:#24292f;color:#e6e6e6;border:1px solid #3c414d}"
".d{font-size:.85rem;color:#c3c8d0;line-height:1.5;margin:0 0 .6rem}";

static const char ABOUT_OPEN[] = "</style></head><body>";

static const char ABOUT_BODY[] =
"<h1>About SpiderBridge</h1>"
"<p class=sub>Local Home Assistant MQTT bridge for Spider Farmer GGS controllers</p>"
"<fieldset><legend>Project</legend>"
"<div class=row><span class=lbl>Repository</span>"
"<a class='b gh' href=https://github.com/matthias1232/spider-farmer-homeassistant>"
"spider-farmer-homeassistant</a></div>"
"<div class=row><span class=lbl>Documentation</span>"
"<a href=https://matthias1232.github.io/spider-farmer-homeassistant/>"
"matthias1232.github.io/spider-farmer-homeassistant</a></div>"
"<div class=row><span class=lbl>Web installer</span>"
"<a href=https://matthias1232.github.io/spider-farmer-homeassistant/installer/>"
"matthias1232.github.io/spider-farmer-homeassistant/installer</a></div>"
"<div class=row><span class=lbl>Author</span><span class=v>matthias1232</span></div>"
"<div class=row><span class=lbl>License</span><span class=v>GPL-3.0-or-later</span></div>"
"</fieldset>"
"<fieldset><legend>Support</legend>"
"<p class=d>SpiderBridge is free open-source software. If it helps you, you can "
"support ongoing development and maintenance:</p>"
"<a class='b bmc' href=https://www.buymeacoffee.com/matthias1232>"
"buymeacoffee.com/matthias1232</a>"
"</fieldset>"
"<fieldset><legend>Firmware</legend>"
"<div class=row><span class=lbl>Version</span><span class=v id=fwver>&hellip;</span></div>"
"<div class=row><span class=lbl>Built</span><span class=v id=fwbuilt>&hellip;</span></div>"
"</fieldset>"
"<script>"
"fetch('/status/data').then(r=>r.json()).then(d=>{const f=d.fw||{};"
"if(f.version)document.getElementById('fwver').textContent=f.version;"
"if(f.built)document.getElementById('fwbuilt').textContent=f.built;});"
"</script>";

esp_err_t about_page_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, ABOUT_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NAV_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, ABOUT_OPEN, HTTPD_RESP_USE_STRLEN);
    nav_send(req, NAV_ABOUT);
    httpd_resp_send_chunk(req, ABOUT_BODY, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}
