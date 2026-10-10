#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "sb_config.h"
#include "provisioning.h"
#include "wifi_apsta.h"
#include "ha_mqtt.h"
#include "mitm_proxy.h"
#include "device_registry.h"
#include "time_sync.h"
#include "sf_command_handler.h"
#include "sys_log.h"
#include "syslog_fwd.h"
#include "ota_update.h"
#include "wan_gate.h"
#include "web_auth.h"
#include "status_page.h"
#include "nav.h"
#include "ggs_ble.h"
#include "supervisor.h"

static const char STATUS_PAGE[] =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>SpiderBridge Status</title><style>"
"*{box-sizing:border-box}"
"body{font-family:system-ui,sans-serif;max-width:46rem;margin:0 auto;"
"padding:1rem;background:#13151a;color:#e6e6e6}"
"h1{font-size:1.3rem;margin:0 0 .2rem}"
"p.sub{margin:0 0 1rem;color:#9aa0aa;font-size:.85rem}"
"a{color:#8ab4f8;text-decoration:none}"
"fieldset{border:1px solid #2c303a;border-radius:.5rem;padding:.9rem;"
"margin:0 0 1rem;background:#1a1d24}"
"legend{color:#8ab4f8;font-size:.9rem;padding:0 .4rem}"
".row{display:flex;gap:.6rem;padding:.3rem 0;font-size:.85rem;"
"border-bottom:1px solid #22262e;flex-wrap:wrap;align-items:center}"
".row:last-child{border-bottom:0}"
".lbl{flex:1;min-width:10rem;color:#9aa0aa}"
".v{font-family:ui-monospace,monospace;font-size:.82rem}"
".ok{color:#4ade80}.bad{color:#f87171}.warn{color:#fbbf24}"
".bar{width:7rem;height:.45rem;background:#2c303a;border-radius:.25rem;"
"overflow:hidden}"
".bar i{display:block;height:100%;background:#4ade80}"
".sw{position:relative;display:inline-block;width:2.4rem;height:1.3rem}"
".sw input{opacity:0;width:0;height:0}"
".sw span{position:absolute;inset:0;background:#333845;border-radius:1rem;"
"cursor:pointer;transition:.15s}"
".sw span:before{content:'';position:absolute;width:1rem;height:1rem;"
"left:.15rem;top:.15rem;background:#9aa0aa;border-radius:50%;"
"transition:.15s}"
".sw input:checked+span{background:#8ab4f8}"
".sw input:checked+span:before{transform:translateX(1.1rem);background:#fff}"
".sw input:disabled+span{opacity:.4;cursor:not-allowed}"
"#log{background:#0f1116;border:1px solid #2c303a;border-radius:.4rem;"
"padding:.6rem;font-family:ui-monospace,monospace;font-size:.75rem;"
"line-height:1.45;height:22rem;overflow-y:auto;white-space:pre-wrap;"
"word-break:break-word}"
".t{color:#6b7280}";

// Closes the <style> block and opens <body>, and nothing more.
//
// The nav bar has to be written immediately after <body> opens. An
// earlier version lumped the whole rest of the page in here and sent
// the nav afterwards, which put it after </html> — the browser then
// showed it as unstyled stray text at the very bottom instead of a bar
// at the top.
static const char STATUS_BODY_OPEN[] =
"</style></head><body>";

static const char STATUS_PAGE_BODY[] =
"<h1>Status</h1>"
"<div id=\"app\"><div class=\"sub\">Loading&hellip;</div></div>"
"<fieldset><legend>System log</legend>"
"<div id=\"log\">Loading&hellip;</div></fieldset>"
"<script>"
"function esc(s){return String(s).replace(/[<>&]/g,c=>"
"({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]));}"
"function row(l,v,cls){return '<div class=row><span class=lbl>'+l+'</span>'+"
"'<span class=\"v '+(cls||'')+'\">'+esc(v)+'</span></div>';}"
"function dur(s){"
"if(s<90)return s+'s';"
"if(s<5400)return Math.floor(s/60)+'m';"
"if(s<172800)return Math.floor(s/3600)+'h';"
"return Math.floor(s/86400)+'d';}"
"function render(S){"
"let h='';"
// --- Uplink ---
"const w=S.wifi;"
"h+='<fieldset><legend>Uplink</legend>';"
"if(!w.sta_enabled){h+=row('State','No home network configured','warn');}"
"else if(w.sta_has_ip){"
"h+=row('State','Connected','ok');"
"h+=row('Network',w.ssid);"
"h+=row('Access point',w.bssid+'  ch '+w.channel);"
"h+='<div class=row><span class=lbl>Signal</span>'+"
"'<span class=\"v '+(w.quality>60?'ok':w.quality>30?'warn':'bad')+'\">'+"
"w.rssi+' dBm ('+w.quality+'%)</span>'+"
"'<span class=bar><i style=\"width:'+w.quality+'%\"></i></span></div>';"
"h+=row('IP address',w.ip);"
"h+=row('Netmask',w.netmask);"
"h+=row('Gateway',w.gateway);"
"h+=row('DNS',w.dns);"
"}else{h+=row('State','Not connected','bad');"
"h+=row('Configured network',w.ssid||'(set in Settings)');}"
"if(w.disconnects)h+=row('Drops since boot',w.disconnects,"
"w.disconnects>5?'warn':'');"
"h+='</fieldset>';"
// --- Hotspot ---
"h+='<fieldset><legend>Hotspot</legend>';"
"h+=row('Network',w.ap_ssid);"
"h+=row('Address',w.ap_ip);"
"if(w.ap_subnet)h+=row('Subnet',w.ap_subnet);"
// Shown with the uplink channel beside it, because they are always the
// same by necessity: one radio cannot serve two channels. Seeing them
// together makes that obvious instead of looking like a coincidence.
"h+=row('Channel',w.ap_channel+(w.sta_has_ip&&w.channel===w.ap_channel?"
"' (shared with the uplink)':''));"
"h+=row('MAC',w.ap_mac);"
"h+=row('Clients connected',w.ap_clients,w.ap_clients?'ok':'');"
"h+=row('Internet for clients',S.wan?'On':'Off',S.wan?'ok':'warn');"
"h+='</fieldset>';"
// --- Broker ---
"const m=S.mqtt;"
"h+='<fieldset><legend>MQTT broker</legend>';"
"if(!m.configured){h+=row('State','No broker configured','warn');}"
"else{"
"h+=row('State',m.connected?'Connected':'Not connected',"
"m.connected?'ok':'bad');"
"h+=row('Broker',m.broker);"
"if(m.connected)h+=row('Connected for',dur(m.uptime_s));"
"h+=row('Messages sent',m.publishes);"
"if(m.disconnects)h+=row('Drops since boot',m.disconnects,"
"m.disconnects>5?'warn':'');"
"if(m.last_error)h+=row('Last error',m.last_error,'bad');}"
"h+='</fieldset>';"
// --- Syslog server ---
"const sl=S.syslog;"
"if(sl){h+='<fieldset><legend>Syslog server</legend>';"
"if(!sl.enabled){h+=row('State','Not configured','');}"
"else{"
"const pn=['UDP','TCP','TLS'][sl.proto]||'?';"
// UDP has no connection to report, only whether lines are going out.
"h+=row('State',sl.proto===0?(sl.connected?'Sending':'Not sending')"
":(sl.connected?'Connected':'Not connected'),sl.connected?'ok':'bad');"
"h+=row('Server',sl.host+':'+sl.port+' ('+pn+')');"
"h+=row('Lines sent',sl.sent);"
"if(sl.dropped)h+=row('Lines dropped',sl.dropped,'warn');"
"if(sl.proto&&sl.reconnects>1)h+=row('Reconnects',sl.reconnects-1,"
"sl.reconnects>6?'warn':'');"
"if(sl.last_error)h+=row('Last error',sl.last_error,'bad');}"
"h+='</fieldset>';}"
// --- Controllers ---
"h+='<fieldset><legend>Controllers</legend>';"
"if(!S.devices.length){h+=row('None known yet',"
"'A controller appears once it connects','warn');}"
"else{for(const d of S.devices){"
"let note=d.online?'Connected':'Not connected';"
// Keepalive detail, which is what shows a connection is genuinely alive
// rather than just an open socket.
"if(d.online&&d.keepalive_s)"
"note+='  last heard '+d.silence_s+'s ago, keepalive '+d.keepalive_s+'s';"
"else if(d.online&&d.silence_s!==undefined)"
"note+='  last heard '+d.silence_s+'s ago';"
"h+=row(d.name,note+'  '+d.mac,d.online?'ok':'');}}"
"h+='</fieldset>';"
// --- Clock ---
"const t=S.time;"
"h+='<fieldset><legend>Clock</legend>';"
"h+=row('Local time',t.now,t.synced?'ok':'warn');"
"h+=row('Synchronised',t.synced?'Yes':'Not yet',t.synced?'ok':'warn');"
"h+=row('Time zone',t.name||t.zone);"
"h+=row('Rules',t.zone);"
"h+=row('In force',t.abbrev+'  UTC'+(t.utc_offset_min<0?'-':'+')+"
"Math.floor(Math.abs(t.utc_offset_min)/60)+':'+"
"String(Math.abs(t.utc_offset_min)%60).padStart(2,'0')+"
"(t.is_dst?'  (daylight saving)':''));"
"h+=row('NTP server',t.server);"
// Daylight saving, switchable here rather than only in Settings.
//
// Greyed out while automatic is ticked: the zone's own rules decide,
// and a switch that silently does nothing is worse than none.
"h+='<div class=row><span class=lbl>Summer time</span>'+"
"'<label class=sw><input type=checkbox id=dstsw '+"
"(t.dst_mode==2?'checked ':'')+(t.dst_mode==0?'disabled ':'')+"
"'onchange=\"setDst()\"><span></span></label>'+"
"'<span class=\"v '+(t.dst_mode==0?'':'ok')+'\">'+"
"(t.dst_mode==0?'decided by the zone rules':"
"(t.dst_mode==2?'forced on':'forced off'))+'</span></div>';"
"h+='<div class=row><span class=lbl>Follow the zone rules</span>'+"
"'<label class=sw><input type=checkbox id=dstauto '+"
"(t.dst_mode==0?'checked ':'')+'onchange=\"setAuto()\"><span></span>"
"</label><span class=v>automatic</span></div>';"
"if(S.tz_push)h+='<div class=row><span class=lbl></span>"
"<span class=\"v ok\">Every controller follows these settings</span>"
"</div>';"
"h+='</fieldset>';"
// --- Firmware ---
"const f=S.fw;"
"h+='<fieldset><legend>Firmware</legend>';"
"h+=row('Version',f.version);"
"h+=row('Built',f.built);"
"h+=row('Running slot',f.slot);"
"if(f.ota_url){"
"h+=row('Update check',f.ota_checked?"
"(f.ota_available?'Newer version available: '+f.ota_remote:'Up to date')"
":'Not run yet',f.ota_available?'warn':'ok');"
"h+=row('Last checked',f.ota_last);"
"if(f.ota_error)h+=row('Check error',f.ota_error,'bad');}"
"h+='</fieldset>';"
// --- Device ---
"h+='<fieldset><legend>Device</legend>';"
"h+=row('Uptime',dur(S.uptime_s));"
"h+=row('Free memory',Math.round(S.heap_free/1024)+' KB',"
"S.heap_free<40000?'warn':'');"
"h+=row('Lowest ever',Math.round(S.heap_min/1024)+' KB',"
"S.heap_min<25000?'warn':'');"
"h+=row('Restart reason',S.reset_reason);"
"if(S.boots!==undefined)h+=row('Starts / crashes',S.boots+' / '+S.crashes,S.crashes>0?'warn':'');"
"if(S.crash_streak>0)h+=row('Restarts without a healthy run',S.crash_streak+' of 3','warn');"
"if(S.sv_why)h+=row('Last self-restart',['','a task stopped responding','memory ran low','Bluetooth step stalled','requested','uplink was down'][S.sv_why]+(S.sv_task?' ('+S.sv_task+')':''),'warn');"
"if(S.safe_mode)h+=row('Mode','SAFE MODE: the last starts ended in a crash. Only hotspot, web interface and USB run. Update the firmware or restart from here.','bad');"
"h+='</fieldset>';"
// --- Bluetooth ---
"if(S.ble){const b=S.ble;h+='<fieldset><legend>Bluetooth</legend>';"
"if(b.pending)h+=row('State','Restarting into the Bluetooth-only boot','warn');"
"else if(b.released)h+=row('State','Off (memory released to the system)','ok');"
"else h+=row('State','Memory not released','bad');"
"if(b.last)h+=row('Last job',b.last);"
"h+='<div class=row><small>Bluetooth never runs during normal operation: its memory is '+"
"'needed for the controllers\\' encrypted connections. Scanning or setting up a controller '+"
"'(Settings) restarts the bridge into a short Bluetooth-only boot, which does the job and '+"
"'restarts straight back with Bluetooth off.</small></div>';"
"h+='</fieldset>';}"
"document.getElementById('app').innerHTML=h;}"
// Both pollers back off when the bridge reports memory pressure, and
// recover as soon as a normal reply arrives. Polling straight through a
// tight heap is what previously turned memory pressure into a reboot.
// Daylight saving, applied immediately.
//
// With "apply to every controller" on, the sync task pushes the change
// to each one within a few seconds, so one switch covers them all.
"async function setMode(m){"
"try{await fetch('/status/dst',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'mode='+m});setTimeout(load,500);}catch(e){}}"
"function setDst(){"
"setMode(document.getElementById('dstsw').checked?2:1);}"
"function setAuto(){"
"if(document.getElementById('dstauto').checked)setMode(0);"
"else setMode(document.getElementById('dstsw').checked?2:1);}"
"let swait=3000;"
"async function load(){"
// A hidden tab does not poll; it checks again shortly and resumes.
"if(document.hidden){setTimeout(load,3000);return;}"
"try{const r=await fetch('/status/data');"
"if(r.status===503){setTimeout(load,1500);return;}"
"const j=await r.json();"
"if(j.busy){swait=Math.min(swait*2,20000);}"
"else{swait=3000;render(j);}}"
"catch(e){swait=Math.min(swait*2,20000);}"
"setTimeout(load,swait);}"
"let seq=0,atEnd=true;"
"const lg=document.getElementById('log');"
"lg.addEventListener('scroll',()=>{"
"atEnd=lg.scrollTop+lg.clientHeight>=lg.scrollHeight-30;});"
"let lwait=2500;"
"async function loadLog(){"
"if(document.hidden){setTimeout(loadLog,3000);return;}"
"try{const r=await fetch('/syslog/data?after='+seq);const j=await r.json();"
"if(j.busy){lwait=Math.min(lwait*2,20000);setTimeout(loadLog,lwait);return;}"
"lwait=2500;"
"if(seq===0)lg.textContent='';"
"for(const e of j.lines){"
"const d=document.createElement('div');"
"const s=document.createElement('span');"
"s.className='t';s.textContent='['+(e.ms/1000).toFixed(1)+'] ';"
"d.appendChild(s);d.appendChild(document.createTextNode(e.text));"
"lg.appendChild(d);seq=e.seq;}"
"while(lg.childNodes.length>400)lg.removeChild(lg.firstChild);"
"if(atEnd)lg.scrollTop=lg.scrollHeight;}"
"catch(e){lwait=Math.min(lwait*2,20000);}"
"setTimeout(loadLog,lwait);}"
"load();loadLog();"
"</script></body></html>";

esp_err_t status_page_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, STATUS_PAGE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NAV_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, STATUS_BODY_OPEN, HTTPD_RESP_USE_STRLEN);
    nav_send(req, NAV_STATUS);
    httpd_resp_send_chunk(req, STATUS_PAGE_BODY, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static const char *reset_reason_text(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "Power on";
        case ESP_RST_SW:       return "Software restart";
        case ESP_RST_PANIC:    return "Crash";
        case ESP_RST_INT_WDT:  return "Interrupt watchdog";
        case ESP_RST_TASK_WDT: return "Task watchdog";
        case ESP_RST_WDT:      return "Watchdog";
        case ESP_RST_BROWNOUT: return "Power dipped (brownout)";
        case ESP_RST_DEEPSLEEP:return "Woke from deep sleep";
        case ESP_RST_EXT:      return "External reset";
        default:               return "Unknown";
    }
}

static esp_err_t status_data_impl(httpd_req_t *req);

esp_err_t status_data_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    if (!web_heavy_begin(req)) return ESP_OK;
    esp_err_t r = status_data_impl(req);
    web_heavy_end();
    return r;
}

static esp_err_t status_data_impl(httpd_req_t *req)
{

    cJSON *root = cJSON_CreateObject();

    // --- Wi-Fi ---
    wifi_status_t w;
    wifi_apsta_get_status(&w);

    cJSON *jw = cJSON_CreateObject();
    cJSON_AddBoolToObject(jw, "sta_enabled", w.sta_enabled);
    cJSON_AddBoolToObject(jw, "sta_has_ip", w.sta_has_ip);
    cJSON_AddStringToObject(jw, "ssid", w.sta_ssid);
    cJSON_AddStringToObject(jw, "bssid", w.sta_bssid);
    cJSON_AddNumberToObject(jw, "channel", w.sta_channel);
    cJSON_AddNumberToObject(jw, "rssi", w.sta_rssi);
    cJSON_AddNumberToObject(jw, "quality", w.sta_quality);
    cJSON_AddStringToObject(jw, "ip", w.sta_ip);
    cJSON_AddStringToObject(jw, "netmask", w.sta_netmask);
    cJSON_AddStringToObject(jw, "gateway", w.sta_gateway);
    cJSON_AddStringToObject(jw, "dns", w.sta_dns);
    cJSON_AddNumberToObject(jw, "disconnects", w.sta_disconnects);
    cJSON_AddStringToObject(jw, "ap_ssid", w.ap_ssid);
    cJSON_AddStringToObject(jw, "ap_ip", w.ap_ip);
    {
        sb_prov_cfg_t *pc = malloc(sizeof(*pc));
        if (pc) {
            char sn[24];
            sb_prov_load(pc);
            sb_prov_ap_subnet_str(pc, sn, sizeof(sn));
            cJSON_AddStringToObject(jw, "ap_subnet", sn);
            free(pc);
        }
    }
    cJSON_AddNumberToObject(jw, "ap_channel", w.ap_channel);
    cJSON_AddNumberToObject(jw, "ap_clients", w.ap_clients);
    cJSON_AddStringToObject(jw, "ap_mac", w.ap_mac);
    cJSON_AddItemToObject(root, "wifi", jw);

    cJSON_AddBoolToObject(root, "wan", wan_gate_is_open());

    // --- Broker ---
    ha_mqtt_status_t m;
    ha_mqtt_get_status(&m);

    cJSON *jm = cJSON_CreateObject();
    cJSON_AddBoolToObject(jm, "configured", m.configured);
    cJSON_AddBoolToObject(jm, "connected", m.connected);
    cJSON_AddStringToObject(jm, "broker", m.broker);
    cJSON_AddStringToObject(jm, "last_error", m.last_error);
    cJSON_AddNumberToObject(jm, "connects", m.connects);
    cJSON_AddNumberToObject(jm, "disconnects", m.disconnects);
    cJSON_AddNumberToObject(jm, "publishes", m.publishes);
    cJSON_AddNumberToObject(jm, "uptime_s", m.uptime_s);
    cJSON_AddItemToObject(root, "mqtt", jm);

    // --- Syslog forwarding ---
    {
        syslog_fwd_status_t sl;
        syslog_fwd_status(&sl);
        cJSON *js = cJSON_CreateObject();
        cJSON_AddBoolToObject(js, "enabled", sl.enabled);
        cJSON_AddBoolToObject(js, "connected", sl.connected);
        cJSON_AddNumberToObject(js, "proto", sl.proto);
        cJSON_AddStringToObject(js, "host", sl.host);
        cJSON_AddNumberToObject(js, "port", sl.port);
        cJSON_AddNumberToObject(js, "sent", sl.sent);
        cJSON_AddNumberToObject(js, "dropped", sl.dropped);
        cJSON_AddNumberToObject(js, "reconnects", sl.reconnects);
        cJSON_AddStringToObject(js, "last_error", sl.last_error);
        cJSON_AddItemToObject(root, "syslog", js);
    }

    // Connection history. Written as a flat array of stamped transitions
    // so the diagnosis of a flapping session does not depend on catching
    // the system log in the one second before the MQTT noise floods it.
    ha_mqtt_hist_t hist[16];
    int hn = ha_mqtt_history(hist, 16);
    if (hn > 0) {
        cJSON *jh = cJSON_CreateArray();
        for (int i = 0; i < hn; i++) {
            cJSON *je = cJSON_CreateObject();
            cJSON_AddNumberToObject(je, "s", hist[i].seq);
            cJSON_AddNumberToObject(je, "ms", hist[i].ms);
            char w[2] = { hist[i].what, 0 };
            cJSON_AddStringToObject(je, "w", w);
            cJSON_AddNumberToObject(je, "d", hist[i].detail);
            cJSON_AddNumberToObject(je, "p", hist[i].payload);
            cJSON_AddNumberToObject(je, "pub", hist[i].publishes);
            cJSON_AddItemToArray(jh, je);
        }
        cJSON_AddItemToObject(jm, "hist", jh);
    }

    // --- Controllers ---
    cJSON *jd = cJSON_CreateArray();
    device_registry_lock();
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        device_entry_t *d = device_registry_at(i);
        if (!d) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", d->name);
        cJSON_AddStringToObject(e, "mac", d->mac);
        cJSON_AddStringToObject(e, "slug", d->slug);
        cJSON_AddBoolToObject(e, "online", d->online);

        // Keepalive health: how long the controller has been quiet and
        // what interval it keeps. This distinguishes a device that is
        // simply idle from one that has actually gone away.
        uint32_t silence = 0, keepalive = 0;
        mitm_proxy_liveness(d->mac, &silence, &keepalive);
        cJSON_AddNumberToObject(e, "silence_s", silence);
        cJSON_AddNumberToObject(e, "keepalive_s", keepalive);

        cJSON_AddItemToArray(jd, e);
    }
    device_registry_unlock();
    cJSON_AddItemToObject(root, "devices", jd);

    // --- Clock ---
    time_status_t t;
    time_sync_get_status(&t);

    cJSON *jt = cJSON_CreateObject();
    cJSON_AddBoolToObject(jt, "synced", t.synced);
    cJSON_AddStringToObject(jt, "now", t.now);
    cJSON_AddStringToObject(jt, "zone", t.zone);
    cJSON_AddStringToObject(jt, "abbrev", t.abbrev);
    cJSON_AddBoolToObject(jt, "is_dst", t.is_dst);
    cJSON_AddNumberToObject(jt, "utc_offset_min", t.utc_offset_min);
    cJSON_AddStringToObject(jt, "server", t.server);
    cJSON_AddStringToObject(jt, "name", prov_tz_name());
    cJSON_AddNumberToObject(jt, "dst_mode", prov_dst_mode());
    cJSON_AddItemToObject(root, "time", jt);
    cJSON_AddBoolToObject(root, "tz_push", prov_tz_push());

    // --- Firmware ---
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    ota_remote_status_t o;
    ota_remote_get_status(&o);
    sb_prov_cfg_t prov;
    sb_prov_load(&prov);

    char built[48];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);

    cJSON *jf = cJSON_CreateObject();
    cJSON_AddStringToObject(jf, "version", app->version);
    cJSON_AddStringToObject(jf, "built", built);
    cJSON_AddStringToObject(jf, "slot", running ? running->label : "?");
    cJSON_AddStringToObject(jf, "ota_url", prov.ota_url);
    cJSON_AddBoolToObject(jf, "ota_checked", o.checked);
    cJSON_AddBoolToObject(jf, "ota_available", o.available);
    cJSON_AddStringToObject(jf, "ota_remote", o.remote_version);
    cJSON_AddStringToObject(jf, "ota_last", o.last_check);
    cJSON_AddStringToObject(jf, "ota_error", o.last_error);
    cJSON_AddItemToObject(root, "fw", jf);

    // --- Device ---
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "heap_free",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "heap_min",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddStringToObject(root, "heap_low_note", ha_mqtt_heap_low_note());
    cJSON_AddNumberToObject(root, "heap_largest",
                            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    {
        uint32_t served, shed, busy;
        web_stats(&served, &shed, &busy);
        cJSON *w = cJSON_AddObjectToObject(root, "web");
        cJSON_AddNumberToObject(w, "served", served);
        cJSON_AddNumberToObject(w, "shed", shed);
        cJSON_AddNumberToObject(w, "busy", busy);
    }
    // Lowest free stack seen per task (bytes), so a task that comes close to
    // overflowing shows up before it crashes.
    {
        static const char *const TASKS[] = { "mqtt_task", "sb_session", "httpd", "config_poll", "devsave" };
        cJSON *st = cJSON_AddObjectToObject(root, "stack_free");
        for (size_t i = 0; i < sizeof(TASKS) / sizeof(TASKS[0]); i++) {
            TaskHandle_t t = xTaskGetHandle(TASKS[i]);
            if (t) cJSON_AddNumberToObject(st, TASKS[i], uxTaskGetStackHighWaterMark(t));
        }
    }
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_text());
    {
        sv_info_t *si = calloc(1, sizeof(*si));
        if (si) {
            sv_get_info(si);
            cJSON_AddNumberToObject(root, "boots", si->boots);
            cJSON_AddNumberToObject(root, "crashes", si->crashes);
            cJSON_AddNumberToObject(root, "crash_streak", si->crash_streak);
            cJSON_AddBoolToObject(root, "safe_mode", si->safe_mode);
            cJSON_AddNumberToObject(root, "sv_why", (int)si->last_sv_why);
            cJSON_AddStringToObject(root, "sv_task", si->last_sv_task);
            free(si);
        }
    }
    {
        ggs_ble_status_t *bs = malloc(sizeof(*bs));
        cJSON *jb = cJSON_AddObjectToObject(root, "ble");
        cJSON_AddBoolToObject(jb, "released", ggs_ble_memory_released());
        if (bs) {
            ggs_ble_get_status(bs);
            cJSON_AddBoolToObject(jb, "pending", bs->pending);
            cJSON_AddNumberToObject(jb, "found", bs->count);
            cJSON_AddStringToObject(jb, "last", bs->last_result);
            free(bs);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        // Transient memory pressure, not a malformed request. Reporting
        // it as 503 keeps a polling client from treating it as fatal.
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"busy\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, strlen(json));
    cJSON_free(json);
    return err;
}

// ---------------------------------------------------------------------------
// POST /status/dst
//
// Switches daylight saving. Takes effect on the bridge at once, and on
// every controller within a few seconds when the sync setting is on.
// ---------------------------------------------------------------------------
esp_err_t status_dst_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    char body[48] = "";
    if (req->content_len > 0 && req->content_len < (int)sizeof(body)) {
        int n = httpd_req_recv(req, body, req->content_len);
        if (n > 0) body[n] = '\0';
    }

    const char *eq = strstr(body, "mode=");
    if (!eq) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Missing mode");
    }

    int mode = atoi(eq + 5);
    if (mode < SB_DST_AUTO || mode > SB_DST_SUMMER) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Unknown mode");
    }

    prov_set_dst_mode(mode);
    time_sync_apply_tz();

    // Push to the controllers straight away rather than waiting for the
    // sync task, so the change is visible immediately.
    if (prov_tz_push()) {
        sf_push_timezone_all();
    }

    ha_mqtt_publish_bridge_state();

    ESP_LOGI("status", "Daylight saving set to %s",
             mode == SB_DST_AUTO     ? "automatic" :
             mode == SB_DST_STANDARD ? "always standard" : "always summer");

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t syslog_data_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    uint32_t after = 0;
    char q[48];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "after", v, sizeof(v)) == ESP_OK) {
            after = (uint32_t)strtoul(v, NULL, 10);
        }
    }

    // Streamed one line at a time.
    //
    // Fetching 60 entries at once meant a single ~10 KB allocation plus
    // a cJSON tree of the same size, on every poll. With two TLS
    // sessions the heap is both small and fragmented, and the failure
    // mode was not a graceful error: the browser retried every second
    // and the device eventually rebooted.
    const int MAX = 60;

    sys_log_entry_t *entry = malloc(sizeof(sys_log_entry_t));
    char *chunk = malloc(SYSLOG_LINE_MAX * 2 + 128);
    if (!entry || !chunk) {
        free(entry); free(chunk);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"lines\":[],\"busy\":true}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"lines\":[", HTTPD_RESP_USE_STRLEN);

    uint32_t cursor = after;
    int n = 0;
    for (int i = 0; i < MAX; i++) {
        if (sys_log_fetch(cursor, entry, 1) != 1) break;
        cursor = entry->seq;

        // Escape only what JSON requires; log lines are already
        // stripped of control characters by sys_log.c.
        char esc[SYSLOG_LINE_MAX * 2];
        size_t o = 0;
        for (size_t k = 0; entry->text[k] && o + 2 < sizeof(esc); k++) {
            char c = entry->text[k];
            if (c == '"' || c == '\\') esc[o++] = '\\';
            esc[o++] = c;
        }
        esc[o] = '\0';

        int len = snprintf(chunk, SYSLOG_LINE_MAX * 2 + 128,
                           "%s{\"seq\":%u,\"ms\":%u,\"text\":\"%s\"}",
                           n ? "," : "", (unsigned)entry->seq,
                           (unsigned)entry->ms, esc);
        if (len > 0 && len < SYSLOG_LINE_MAX * 2 + 128) {
            httpd_resp_send_chunk(req, chunk, len);
            n++;
        }
    }

    int len = snprintf(chunk, SYSLOG_LINE_MAX * 2 + 128,
                       "],\"dropped\":%u}", (unsigned)sys_log_dropped());
    httpd_resp_send_chunk(req, chunk, len);
    httpd_resp_send_chunk(req, NULL, 0);

    free(entry); free(chunk);
    return ESP_OK;
}
