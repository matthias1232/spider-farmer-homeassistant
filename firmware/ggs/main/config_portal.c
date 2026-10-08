#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include "esp_wifi.h"
#include "wifi_apsta.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sb_config.h"
#include "provisioning.h"
#include "live_log.h"
#include "sys_log.h"
#include "syslog_fwd.h"
#include "dns_hijack.h"
#include "web_auth.h"
#include "control_page.h"
#include "status_page.h"
#include "ota_update.h"
#include "mqtt_send.h"
#include "sb_system.h"
#include "network_page.h"
#include "tz_table.h"
#include "nav.h"
#include "wan_gate.h"
#include "time_sync.h"
#include "ha_mqtt.h"
#include "sf_command_handler.h"
#include "ggs_ble.h"
#include "mac_filter.h"
#include "device_registry.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "config_portal.h"

static const char *TAG = "config_portal";

// ---------------------------------------------------------------------------
// Tiny form helpers
// ---------------------------------------------------------------------------

static void url_decode(const char *src, char *dst, size_t dst_sz)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_sz; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) &&
                                      isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_sz)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *eq = strchr(p, '=');
        if (eq && (!amp || eq < amp) &&
            (size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            // Decoded straight from the body into the caller's buffer.
            // Going through a fixed 256-byte copy first silently cut every
            // pasted CA certificate down to its first 256 encoded bytes,
            // so a stored certificate could never verify anything.
            size_t raw_len = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
            char *raw = malloc(raw_len + 1);
            if (!raw) { if (out_sz) out[0] = '\0'; return true; }
            memcpy(raw, eq + 1, raw_len);
            raw[raw_len] = '\0';
            url_decode(raw, out, out_sz);
            free(raw);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

static void html_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0'; si++) {
        const char *rep = NULL;
        switch (src[si]) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            default: break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (di + rl + 1 > dst_sz) break;
            memcpy(dst + di, rep, rl);
            di += rl;
        } else {
            if (di + 2 > dst_sz) break;
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

// ---------------------------------------------------------------------------
// GET /
// ---------------------------------------------------------------------------

static const char PAGE_STYLE[] =
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>SpiderBridge</title><style>"
    "*{box-sizing:border-box}"
    "body{font-family:system-ui,sans-serif;max-width:32rem;margin:0 auto;"
    "padding:1rem;background:#13151a;color:#e6e6e6}"
    "h1{font-size:1.3rem;margin:0 0 .2rem}"
    "p.sub{margin:0 0 1rem;color:#9aa0aa;font-size:.85rem}"
    "label{display:block;margin:.7rem 0 .2rem;font-size:.85rem;color:#c3c8d0}"
    "input[type=text],input[type=password],input[type=number]{width:100%;"
    "padding:.5rem;background:#0f1116;color:#e6e6e6;"
    "border:1px solid #333845;border-radius:.3rem}"
    "input[type=radio],input[type=checkbox]{width:auto;margin-right:.4rem}"
    "button{width:100%;padding:.7rem;margin-top:1.2rem;background:#8ab4f8;"
    "color:#13151a;font-weight:600;border:0;border-radius:.3rem;"
    "font-size:1rem;cursor:pointer}"
    "fieldset{border:1px solid #2c303a;border-radius:.4rem;margin:1rem 0;"
    "padding:.8rem;background:#1a1d24}"
    "legend{color:#8ab4f8;font-size:.9rem;padding:0 .4rem}"
    "a{color:#8ab4f8;text-decoration:none}"
    "small{display:block;margin-top:.25rem;color:#7d848f;font-size:.78rem}"
    ".info{margin-top:.8rem;padding:.5rem .7rem;background:#0f1116;"
    "border-radius:.35rem;font-size:.8rem;color:#7d848f}"
    // Toggle switch, shared with the status and control pages so the
    // same setting looks the same wherever it appears.
    ".sw{position:relative;display:inline-block;width:2.4rem;height:1.3rem;"
    "flex:none}"
    ".sw input{opacity:0;width:0;height:0}"
    ".sw span{position:absolute;inset:0;background:#333845;"
    "border-radius:1rem;cursor:pointer;transition:.15s}"
    ".sw span:before{content:'';position:absolute;width:1rem;height:1rem;"
    "left:.15rem;top:.15rem;background:#9aa0aa;border-radius:50%;"
    "transition:.15s}"
    ".sw input:checked+span{background:#8ab4f8}"
    ".sw input:checked+span:before{transform:translateX(1.1rem);"
    "background:#fff}"
    ".sw input:disabled+span{opacity:.4;cursor:not-allowed}"
    ".swrow{display:flex;gap:.6rem;align-items:center;margin:.3rem 0}"
    "label.inline{display:flex;gap:.5rem;align-items:center;"
    "margin-top:.5rem;font-weight:400}"
    "label.inline input{width:auto;margin:0}";

// PAGE_STYLE and PAGE_BODY_OPEN are two pieces so NAV_STYLE (shared
// across every page) can be inserted between them without duplicating
// it here.
static const char PAGE_BODY_OPEN[] = "</style></head><body>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    sb_prov_cfg_t p;
    sb_prov_load(&p);

    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    const size_t bs = 2048;
    char e1[192], e2[192], e3[192];
    int n;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, PAGE_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NAV_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, PAGE_BODY_OPEN, HTTPD_RESP_USE_STRLEN);
    nav_send(req, NAV_SETTINGS);
    httpd_resp_send_chunk(req,
        "<h1>SpiderBridge Settings</h1>"
        "<form method=\"POST\" action=\"/save\">"
        // Marks a submit of the whole page. Only then can an absent
        // checkbox be read as "switched off"; see save_post_handler.
        "<input type=hidden name=\"form\" value=\"full\">",
        HTTPD_RESP_USE_STRLEN);

    // --- Networking ---
    html_escape(p.sta_ssid, e1, sizeof(e1));
    n = snprintf(buf, bs,
        "<fieldset><legend>Home network (uplink)</legend>"
        "<label>Wi-Fi SSID</label>"
        "<input type=text id=staSsid name=\"sta_ssid\" value=\"%s\" required>"
        "<button type=button id=wScan>Scan for networks</button>"
        "<div id=wList></div>"
        "<label>Wi-Fi password</label>"
        "<input type=password id=staPass name=\"sta_pass\" placeholder=\"%s\">"
        "<small>Leave empty to keep the stored password.</small>"
        "<button type=button id=wConn>Connect now</button>"
        "<div id=wMsg class=hint></div>"
        "<small>Connect joins the network right away, without a restart, and stores it once it "
        "works; if it fails, the bridge goes back to the previous network. Every step is in "
        "the system log (and syslog).</small>"
        "</fieldset>",
        e1, p.sta_pass[0] ? "saved" : "");
    if (n >= (int)bs) n = bs - 1;
    httpd_resp_send_chunk(req, buf, n);
    httpd_resp_send_chunk(req,
        "<style>#wList .w{display:flex;justify-content:space-between;padding:.35rem .5rem;"
        "margin:.25rem 0;border:1px solid #374151;border-radius:6px;cursor:pointer}"
        "#wList .w:hover{border-color:#60a5fa}#wList small{color:#9ca3af}</style>"
        "<script>(function(){const L=document.getElementById('wList'),M=document.getElementById('wMsg');"
        "const e=s=>String(s).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));"
        "document.getElementById('wScan').onclick=async function(){this.disabled=true;"
        "L.innerHTML='<small>Scanning&hellip;</small>';"
        "try{const a=await (await fetch('/wifi/scan')).json();"
        "L.innerHTML=a.length?a.map(w=>'<div class=w data-s=\"'+e(w.ssid)+'\"><span>'+e(w.ssid)"
        "+(w.open?' <small>(open)</small>':'')+'</span><small>'+w.rssi+' dBm &middot; ch '+w.ch+'</small></div>').join('')"
        ":'<small>No networks found.</small>';"
        "for(const d of L.querySelectorAll('.w'))d.onclick=()=>{document.getElementById('staSsid').value=d.dataset.s;"
        "const pw=document.getElementById('staPass');pw.value='';pw.focus();L.innerHTML='';};"
        "}catch(x){L.innerHTML='<small>Scan failed.</small>';}this.disabled=false;};"
        "document.getElementById('wConn').onclick=async function(){"
        "const s=document.getElementById('staSsid').value.trim(),p=document.getElementById('staPass').value;"
        "if(!s){M.textContent='Enter or pick a network first.';return;}"
        "this.disabled=true;M.textContent='Connecting to '+s+' (up to 20 s)...';"
        "const btn=this;"
        "try{const r=await fetch('/wifi/connect',{method:'POST',headers:{'Content-Type':"
        "'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)});"
        "M.textContent=await r.text();"
        // The attempt runs in the background; poll its result.
        "if(r.status===202){for(let i=0;i<30;i++){await new Promise(z=>setTimeout(z,1500));"
        "try{const j=await (await fetch('/wifi/status')).json();M.textContent=j.msg;if(!j.running)break;}catch(e){}}}"
        "}catch(x){M.textContent='No answer -- if the bridge moved to the new "
        "network, open it at its new address.';}btn.disabled=false;};})();</script>",
        HTTPD_RESP_USE_STRLEN);

    // --- Static address on the home network ---
    html_escape(p.static_ip, e1, sizeof(e1));
    html_escape(p.static_gw, e2, sizeof(e2));
    html_escape(p.static_mask, e3, sizeof(e3));
    n = snprintf(buf, bs,
        "<fieldset><legend>Bridge address on the home network</legend>"
        "<label>IP address</label>"
        "<input type=text name=\"static_ip\" value=\"%s\" "
        "placeholder=\"leave empty for DHCP\">"
        "<label>Gateway</label>"
        "<input type=text name=\"static_gw\" value=\"%s\" "
        "placeholder=\"192.168.1.1\">"
        "<label>Subnet mask</label>"
        "<input type=text name=\"static_mask\" value=\"%s\">"
        "<small>A fixed address keeps this page at the same URL.</small>"
        "</fieldset>",
        e1, e2, e3);
    httpd_resp_send_chunk(req, buf, n);

    // --- Hotspot ---
    char subnet[24];
    sb_prov_ap_subnet_str(&p, subnet, sizeof(subnet));
    html_escape(p.ap_ssid, e2, sizeof(e2));
    html_escape(p.ap_pass, e3, sizeof(e3));
    n = snprintf(buf, bs,
        "<fieldset><legend>Hotspot for the grow controller</legend>"
        "<label>Hotspot SSID</label>"
        "<input type=text name=\"ap_ssid\" value=\"%s\" required>"
        "<label>Hotspot password</label>"
        // Prefilled (the page is behind the admin login) and hidden until
        // asked for; controllers set up by hand need exactly this string.
        "<input type=password id=apPass name=\"ap_pass\" value=\"%s\" minlength=8 autocomplete=off>"
        "<label class=inline><input type=checkbox id=apShow "
        "onchange=\"document.getElementById('apPass').type=this.checked?'text':'password'\">"
        " Show password</label> "
        // Random 15 characters from the browser's crypto RNG, put into the
        // field and shown; nothing changes until the page is saved.
        "<button type=button onclick=\"(function(){"
        "const A='abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789#!';"
        "let p;do{const r=new Uint32Array(15);crypto.getRandomValues(r);"
        "p=Array.from(r,x=>A[x%%A.length]).join('');}"
        "while(!(/[a-z]/.test(p)&&/[A-Z]/.test(p)&&/[0-9]/.test(p)&&/[#!]/.test(p)));"
        "const f=document.getElementById('apPass');f.value=p;f.type='text';"
        "document.getElementById('apShow').checked=true;})()\">Generate password</button>"
        "<small>Generate fills in a strong random password; save the page to use it. "
        "Controllers on the hotspot then need the new password (app or Bluetooth below). "
        "First boot and factory reset use SpiderBridge / 12345678.</small>",
        e2, e3);
    if (n >= (int)bs) n = bs - 1;
    httpd_resp_send_chunk(req, buf, n);
    // Second chunk: one 2 KB buffer is too small for the whole fieldset,
    // and a cut-off chunk loses its </fieldset> -- which put every
    // following section inside this box.
    n = snprintf(buf, bs,
        "<div class=swrow><label class=sw><input type=checkbox name=\"ap_hidden\" value=\"1\"%s>"
        "<span></span></label><span>Hide hotspot name (SSID)</span></div>"
        "<small>The network is no longer shown in Wi-Fi lists. Controllers that already "
        "have its name and password keep connecting. Set controllers up first (app or "
        "Bluetooth below), then hide it; the app's network list will not show it while "
        "hidden. Changing this restarts the bridge.</small>"
        "<label>Hotspot subnet</label>"
        "<input type=text name=\"ap_subnet\" value=\"%s\" "
        "pattern=\"\\d{1,3}(\\.\\d{1,3}){3}/\\d{1,2}\" placeholder=\"192.168.10.0/24\">"
        "<small>Any private network in CIDR form, e.g. 192.168.10.0/24, 10.20.0.0/16 or "
        "172.16.5.0/28 (prefix 8 to 30). The bridge takes the first address, DHCP hands "
        "out the following ones -- as many as the subnet holds, at most 100 (the DHCP "
        "server's lease table). Must not overlap the home network. Changing it restarts the bridge; "
        "this page then moves to the new hotspot address.</small>"
        "</fieldset>",
        p.ap_hidden ? " checked" : "", subnet);
    if (n >= (int)bs) n = bs - 1;
    httpd_resp_send_chunk(req, buf, n);

    // --- Bluetooth GGS setup ---
    //
    // Outside the form's data (its buttons are type=button), so it never
    // affects a settings save. Lists Spider Farmer GGS controllers seen over
    // Bluetooth, which of them are already on the hotspot (and their IP),
    // and a Connect button that hands the others the hotspot's Wi-Fi.
    httpd_resp_send_chunk(req,
        "<style>#bleList .r{display:flex;justify-content:space-between;align-items:center;gap:.6rem;"
        "flex-wrap:wrap;padding:.5rem .6rem;margin:.4rem 0;border:1px solid #374151;border-radius:6px}"
        "#bleList .r:has(input:checked){border-color:#60a5fa}"
        "#bleList .ok{color:#4ade80}#bleList input{width:auto;margin:0 .3rem 0 0}"
        "#bleList small{color:#9ca3af}#bleMsg{margin-top:.5rem}</style>"
        "<fieldset><legend>Add GGS controllers (Bluetooth, optional)</legend>"
        "<p class=hint style=\"margin-top:0\"><b>Optional.</b> All the bridge needs is that "
        "each GGS controller is connected to this bridge's hotspot. If yours already is, or "
        "you prefer the Spider Farmer app, you can ignore this section: in the app, open the "
        "controller's Wi-Fi settings and choose this hotspot's name and password. A controller "
        "already paired with your phone can stay paired -- nothing has to be removed or reset.</p>"
        "<small>Controllers that are not paired with a phone advertise "
        "themselves over Bluetooth (remove one from the Spider Farmer app to "
        "make it visible here). Select one controller and send it this hotspot's name and "
        "password; it joins on its own. The bridge's Bluetooth is off in normal operation: "
        "Scan and Send restart the bridge into a short Bluetooth-only mode (about 30 seconds "
        "to a minute) and back -- controllers on the hotspot drop out for that time and "
        "reconnect by themselves. The list shows the result of the last scan.</small>"
        "<small class=hint>How it works: Bluetooth is never on in normal operation -- the "
        "bridge needs that memory for the controllers' encrypted connections. A scan or a "
        "send restarts the bridge into a Bluetooth-only boot that does that one job, stores "
        "the result and restarts straight back into normal operation with Bluetooth off. "
        "Nothing has to be switched off by hand.</small>"
        "<div id=bleList><small>Loading&hellip;</small></div>"
        "<label class=inline><input type=checkbox id=bleBind> Keep the controller activated after "
        "sending (it then stops advertising over Bluetooth, so no phone can take it over)</label>"
        "<small>Receiving Wi-Fi settings makes a controller stop advertising by itself. Without "
        "this box the bridge deactivates it again right after sending, so it stays visible to "
        "the Spider Farmer app. Deactivate any time with the button below -- or on the Control "
        "page while the controller is on the hotspot.</small>"
        "<button type=button id=bleScan>Scan for controllers</button> "
        "<button type=button id=bleSend disabled>Send Wi-Fi to selected controller</button> "
        "<button type=button id=bleUnpair disabled>Deactivate selected controller (make it visible)</button>"
        "<div id=bleMsg class=hint></div>"
        "<details id=bleDet><summary><small>Log of the last Bluetooth job</small></summary>"
        "<small>Every step: connection, the command as plain JSON, its encryption and the "
        "frame written to the controller, and each reply decrypted. The hotspot password "
        "is shown as *****. <a href=\"/ble/log\" target=_blank>Open as text</a></small>"
        "<pre id=bleTrace style=\"font-size:.72rem;white-space:pre;overflow:auto;max-height:32rem\"></pre>"
        "</details>"
        "</fieldset>"
        "<script>(function(){"
        "const L=document.getElementById('bleList'),M=document.getElementById('bleMsg'),"
        "S=document.getElementById('bleSend'),D=document.getElementById('bleDet'),"
        "U=document.getElementById('bleUnpair');"
        "let sel='',last=null,lastTrace=null;"
        "async function loadLog(){try{document.getElementById('bleTrace').textContent="
        "await (await fetch('/ble/log')).text();}catch(x){}}"
        "D.addEventListener('toggle',()=>{if(D.open)loadLog();});"
        "const e=s=>String(s).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));"
        "async function act(b){try{const r=await fetch('/ble/act',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});"
        "M.textContent=await r.text();}catch(x){M.textContent='Bridge is restarting...';}setTimeout(load,1500);}"
        "function upd(){const off=!sel||!last||last.running;S.disabled=off;U.disabled=off;}"
        "function render(j){let h='';"
        "if(!j.dev.length)h='<small>'+(j.running?'Bluetooth is working&hellip;':(j.scanned?"
        "'No GGS controller found. A controller paired with a phone does not "
        "advertise -- remove it from the Spider Farmer app first.':'Not scanned yet.'))+'</small>';"
        // Selection survives the refresh; it is dropped when the
        // controller is no longer in the list.
        "if(sel&&!j.dev.some(d=>d.addr===sel))sel='';"
        "for(const d of j.dev){"
        "const sess=d.session?' &middot; bridge session active':'';"
        "const st=(j.running&&j.busy===d.addr)?'<span>Setting up&hellip;</span>'"
        ":(d.hs==='ip'?'<span class=ok>Already connected to this hotspot &middot; IP '+e(d.ip)+sess+'</span>'"
        ":(d.hs==='noip'?'<span style=\"color:#f87171\">On the hotspot but no address -- most likely "
        "the old password: send the Wi-Fi settings again</span>'"
        ":(d.adv_wifi?'<span>On another Wi-Fi network</span>'"
        ":(d.known_as?'<span>Known, not on the hotspot right now</span>':'<span>Found via Bluetooth, no Wi-Fi</span>'))));"
        "const det=d.not_seen?'Wi-Fi MAC '+e(d.wifi_mac)+' &middot; BT '+e(d.addr)+' &middot; not seen in the last "
        "scan (paired with a phone, or out of range) -- sending can still be tried'"
        ":e(d.name)+' &middot; BT '+e(d.addr)+' &middot; Wi-Fi MAC '+e(d.wifi_mac||'?')+' &middot; '+d.rssi+' dBm';"
        "h+='<label class=r style=\"cursor:pointer\"><span><input type=radio name=bleSel value=\"'+e(d.addr)+'\"'"
        "+(sel===d.addr?' checked':'')+(j.running?' disabled':'')+'> <b>'+e(d.known_as||d.name||'GGS')+'</b> <small>'"
        "+det+'</small></span>'+st+'</label>';}"
        "L.innerHTML=h;"
        "for(const r of L.querySelectorAll('input[name=bleSel]'))r.onchange=()=>{sel=r.value;upd();};"
        "if(j.result)M.textContent=j.result;"
        // The full log is loaded when opened, and again after each job.
        "if(j.trace!==lastTrace){lastTrace=j.trace;if(D.open)loadLog();}"
        "document.getElementById('bleScan').disabled=j.running;upd();}"
        "async function load(){if(document.hidden){setTimeout(load,5000);return;}"
        "try{last=await (await fetch('/ble/data')).json();render(last);"
        // Faster while Bluetooth works; afterwards keep watching for the
        // controller to appear on the hotspot.
        "setTimeout(load,last.running?1500:5000);}catch(x){setTimeout(load,5000);}}"
        "document.getElementById('bleScan').onclick=()=>{"
        "if(confirm('Scanning restarts the bridge into Bluetooth for about 30 seconds. '"
        "+'Controllers on the hotspot lose their connection for that time. Continue?'))act('a=scan');};"
        "S.onclick=()=>{const d=last&&last.dev.find(x=>x.addr===sel);if(!d)return;"
        "let q='Send the Wi-Fi settings of this bridge to this controller?\\n\\n'"
        "+'Controller: '+(d.known_as||d.name)+'\\nBluetooth: '+d.addr+'\\nWi-Fi MAC: '+(d.wifi_mac||'?')"
        "+'\\nNew network: '+(last.ap_ssid||'?')+'\\n\\n'"
        "+'The controller replaces its stored Wi-Fi with this network and reconnects. '"
        "+'The bridge restarts into Bluetooth for about a minute.';"
        "if(d.hs==='ip')q+='\\n\\nNote: this controller is already connected via Wi-Fi (IP '+d.ip+').';"
        "if(d.hs==='noip')q+='\\n\\nThe controller replaces its stored password with the current one.';"
        "const b=document.getElementById('bleBind').checked;"
        "q+=b?'\\n\\nIt stays paired and stops advertising over Bluetooth.'"
        ":'\\n\\nIt is unpaired again right after, so it stays visible over Bluetooth.';"
        "if(confirm(q))act('a=connect&addr='+encodeURIComponent(d.addr)+'&bind='+(b?1:0));};"
        "U.onclick=()=>{const d=last&&last.dev.find(x=>x.addr===sel);if(!d)return;"
        "if(confirm('Remove the pairing of '+(d.known_as||d.name)+' over Bluetooth?\\n\\nIt then advertises "
        "again and a phone can pair with it. Its Wi-Fi settings stay. The bridge restarts into "
        "Bluetooth for about a minute.\\n\\nIf the controller is on the hotspot, the Unpair button on "
        "the Control page does the same without a restart.'))act('a=unpair&addr='+encodeURIComponent(d.addr));};"
        "load();})();</script>",
        HTTPD_RESP_USE_STRLEN);

    // --- MQTT ---
    //
    // Whole section is optional. The interface steers the controllers
    // through the mirrored cloud session and never touches this broker,
    // so a bridge with no broker still accepts every command from this
    // page -- only Home Assistant will show nothing, which the note says
    // outright rather than leaving an empty field to be guessed about.
    html_escape(p.ha_mqtt_uri, e1, sizeof(e1));
    html_escape(p.ha_mqtt_user, e2, sizeof(e2));
    html_escape(p.device_id, e3, sizeof(e3));
    n = snprintf(buf, bs,
        "<fieldset><legend>MQTT Broker (optional)</legend>"
        "<small>Everything works without a broker: the controllers are "
        "driven straight from this interface. Filling this in only makes "
        "the devices appear in Home Assistant.</small>"
        "<label>Broker URI</label>"
        "<input type=text name=\"ha_uri\" value=\"%s\" "
        "placeholder=\"mqtt://192.168.1.10:1883\">"
        "<label>Username</label>"
        "<input type=text name=\"ha_user\" value=\"%s\">"
        "<label>Password</label>"
        "<input type=password name=\"ha_pass\" placeholder=\"saved\">"
        "<label>Device ID</label>"
        "<input type=text name=\"device_id\" value=\"%s\">"
        "<small>Prefix for every MQTT topic.</small>",
        e1, e2, e3);
    httpd_resp_send_chunk(req, buf, n);

    // Bridge name in Home Assistant: part of the MQTT section, since it
    // only names the bridge device there. One name for the whole bridge,
    // whatever controllers are connected.
    {
        char def[33];
        prov_default_bridge_name(def, sizeof(def));
        bool is_def = prov_bridge_name_is_default();
        html_escape(is_def ? "" : prov_bridge_name(), e1, sizeof(e1));
        html_escape(def, e2, sizeof(e2));
        html_escape(prov_bridge_name(), e3, sizeof(e3));
        n = snprintf(buf, bs,
            "<label>Bridge name in Home Assistant</label>"
            "<input type=text name=\"br_name\" maxlength=32 value=\"%s\" "
            "placeholder=\"%s\">"
            "<small>In use: <b>%s</b>%s. Names the bridge device in Home Assistant; "
            "one name for the whole bridge, whatever controllers are connected. Leave "
            "empty for the default, which ends in this bridge's MAC so several bridges "
            "stay apart. Applied immediately, no restart. Controllers are renamed on "
            "the Control page.</small>"
            "</fieldset>",
            e1, e2, e3, is_def ? " (default)" : "");
    }
    httpd_resp_send_chunk(req, buf, n);

    // --- Remote syslog ---
    //
    // Same layout as the broker's TLS block, so the two read alike. The
    // certificate is never echoed back; only whether one is stored.
    html_escape(p.syslog_host, e1, sizeof(e1));
    n = snprintf(buf, bs,
        "<fieldset><legend>Syslog server (optional)</legend>"
        "<small>Every system log line is also sent here, so the complete "
        "history is kept even though the bridge itself only holds the last "
        "few lines. Applied immediately. Leave the host empty to turn it "
        "off.</small>"
        "<label>Host</label>"
        "<input type=text name=\"slog_host\" value=\"%s\" "
        "placeholder=\"192.168.1.10\">"
        "<label>Transport</label>"
        "<select name=\"slog_proto\">"
        "<option value=0%s>UDP (port 514)</option>"
        "<option value=1%s>TCP (port 514 / 601)</option>"
        "<option value=2%s>TLS, encrypted (port 6514)</option>"
        "</select>"
        "<label>Port</label>"
        "<input type=number name=\"slog_port\" min=1 max=65535 value=\"%d\">"
        "<label>Also send the MQTT log</label>"
        "<select name=\"slog_mqtt\">"
        "<option value=0%s>No, system log only</option>"
        "<option value=1%s>Raw controller traffic</option>"
        "<option value=2%s>Raw controller and Home Assistant traffic</option>"
        "</select>"
        "<small>MQTT packets go into the same stream as the system log, "
        "tagged <code>mqtt</code>, with topic and complete payload.</small>"
        "</fieldset>",
        e1,
        p.syslog_proto == 0 ? " selected" : "",
        p.syslog_proto == 1 ? " selected" : "",
        p.syslog_proto == 2 ? " selected" : "",
        p.syslog_port,
        p.syslog_mqtt == 0 ? " selected" : "",
        p.syslog_mqtt == 1 ? " selected" : "",
        p.syslog_mqtt == 2 ? " selected" : "");
    httpd_resp_send_chunk(req, buf, n);

    n = snprintf(buf, bs,
        "<fieldset><legend>Syslog encryption (TLS)</legend>"
        "<small>Only used with the TLS transport above.</small>"
        "<div class=swrow><label class=sw><input type=checkbox name=\"slog_ins\" value=\"1\"%s><span></span></label><span>"
        "Accept any certificate</span></div>"
        "<small>Connects to syslog servers with a self-signed certificate. "
        "Traffic is encrypted, but the server's identity is not "
        "verified.</small>"
        "<label>CA certificate (optional)</label>"
        "<textarea name=\"slog_ca\" rows=4 spellcheck=\"false\" "
        "placeholder=\"-----BEGIN CERTIFICATE-----\"></textarea>"
        "<small>%s Paste a PEM certificate to verify the server properly; "
        "without one, the built-in list of public CAs is used. Leave empty "
        "to keep the stored one; type a single minus sign to remove "
        "it.</small>"
        "</fieldset>",
        p.syslog_tls_insecure ? " checked" : "",
        sb_prov_syslog_ca_size() > 0 ? "A certificate is stored."
                                     : "None stored.");
    httpd_resp_send_chunk(req, buf, n);

    // --- Broker TLS ---
    //
    // The certificate is never sent back to the browser: showing its
    // length is enough to confirm one is stored, and echoing a stored
    // secret into a page is a habit worth avoiding.
    n = snprintf(buf, bs,
        "<fieldset><legend>Broker encryption (TLS)</legend>"
        "<small>Only used when the URI starts with "
        "<code>mqtts://</code>. Leave this alone for a plain "
        "<code>mqtt://</code> broker.</small>"
        "<div class=swrow><label class=sw><input type=checkbox name=\"mqtt_ins\" value=\"1\"%s><span></span></label><span>"
        "Accept any certificate</span></div>"
        "<small>Connects to brokers with a self-signed certificate. "
        "Traffic is encrypted, but the broker's identity is not "
        "verified.</small>"
        "<label>CA certificate (optional)</label>"
        "<textarea name=\"mqtt_ca\" rows=4 spellcheck=\"false\" "
        "placeholder=\"-----BEGIN CERTIFICATE-----\"></textarea>"
        "<small>%s Paste a PEM certificate to verify the broker "
        "properly. Leave empty to keep the stored one; type a single "
        "minus sign to remove it.</small>"
        "</fieldset>",
        p.mqtt_tls_insecure ? " checked" : "",
        sb_prov_ca_cert_size() > 0 ? "A certificate is stored."
                                   : "None stored.");
    httpd_resp_send_chunk(req, buf, n);

    // --- Publish mode ---
    n = snprintf(buf, bs,
        "<fieldset><legend>What to publish</legend>"
        "<small>Only relevant with a broker. With no broker configured "
        "neither layer publishes anywhere, and the interface still "
        "controls everything.</small>"
        "<label><input type=radio name=\"pubmode\" value=\"1\"%s>"
        "Normalised only</label>"
        "<small>Ready-made entities: switches, sliders, sensors. "
        "Announced via Home Assistant discovery.</small>"
        "<label><input type=radio name=\"pubmode\" value=\"2\"%s>"
        "Raw traffic only</label>"
        "<small>Every controller packet verbatim on "
        "spiderfarmer/&lt;id&gt;/raw.</small>"
        "<label><input type=radio name=\"pubmode\" value=\"3\"%s>Both</label>"
        "<small>Entities plus the raw stream.</small>"
        "</fieldset>",
        p.publish_mode == SB_PUB_HA   ? " checked" : "",
        p.publish_mode == SB_PUB_RAW  ? " checked" : "",
        p.publish_mode == SB_PUB_BOTH ? " checked" : "");
    httpd_resp_send_chunk(req, buf, n);

    // --- Internet access ---
    n = snprintf(buf, bs,
        "<fieldset><legend>Internet access</legend>"
        "<div class=swrow><label class=sw><input type=checkbox name=\"wan\" value=\"1\"%s><span></span></label><span>"
        "Let the controller reach the internet</span></div>"
        "<small>Turns NAT routing for hotspot clients on or off.</small>"
        "<div class=swrow><label class=sw><input type=checkbox name=\"cloudfw\" value=\"1\"%s><span></span></label><span>"
        "Mirror traffic to the Spider Farmer cloud</span></div>"
        "<small>Needed for the vendor app. With this off the bridge "
        "answers the controller itself and nothing leaves your network. "
        "Both have to be off for a fully local setup, because the "
        "mirror uses the bridge's own uplink and bypasses NAT.</small>"
        "<div class=\"info\">Schedule, cycle and PPFD values are only "
        "read from the controller while cloud mirroring is off. With it "
        "on, the cloud closes the session when the bridge queries the "
        "stored configuration, so those entities stay empty until "
        "something is changed in the Spider Farmer app.</div>"
        "</fieldset>",
        p.wan_open ? " checked" : "",
        p.cloud_forward ? " checked" : "");
    httpd_resp_send_chunk(req, buf, n);

    // --- DNS and NTP ---
    char effective[40];
    dns_effective_resolver(effective, sizeof(effective));
    html_escape(p.dns_target, e1, sizeof(e1));
    html_escape(p.ntp_target, e2, sizeof(e2));
    n = snprintf(buf, bs,
        "<fieldset><legend>Name and time service for clients</legend>"
        "<label>DNS server</label>"
        "<input type=text name=\"dns_target\" value=\"%s\" "
        "placeholder=\"from DHCP\">"
        "<div class=swrow><label class=sw><input type=checkbox name=\"dns_rd\" value=\"1\"%s><span></span></label><span>"
        "Send all client lookups to this server</span></div>"
        "<small>Leave the field empty to follow the resolver your router "
        "hands out. Lookups are always forwarded and the real answer "
        "relayed back; only the Spider Farmer cloud name is redirected "
        "to the bridge, which is what makes the proxy work.</small>"
        "<label>Time server (NTP)</label>"
        "<input type=text name=\"ntp_target\" value=\"%s\" "
        "placeholder=\"pool.ntp.org\">"
        "<div class=swrow><label class=sw><input type=checkbox name=\"ntp_rd\" value=\"1\"%s><span></span></label><span>"
        "Redirect client time requests here</span></div>"
        "<small>Point it at a local server to keep the clock correct "
        "with the internet switched off.</small>"
        "<div class=\"info\">Resolver in use: <b>%s</b></div>"
        "</fieldset>",
        e1, p.dns_redirect ? " checked" : "",
        e2, p.ntp_redirect ? " checked" : "",
        effective);
    httpd_resp_send_chunk(req, buf, n);

    // --- Offline exceptions ---
    n = snprintf(buf, bs,
        "<fieldset><legend>Exceptions while internet access is off</legend>"
        "<div class=swrow><label class=sw><input type=checkbox name=\"dns_ex\" value=\"1\"%s><span></span></label><span>"
        "Keep name resolution working</span></div>"
        "<small>Without it the controller cannot resolve anything, "
        "not even the bridge.</small>"
        "<div class=swrow><label class=sw><input type=checkbox name=\"ntp_ex\" value=\"1\"%s><span></span></label><span>"
        "Keep time sync working</span></div>"
        "<small>Without it the clock drifts and schedules fire at the "
        "wrong time.</small>"
        "</fieldset>",
        p.allow_dns_offline ? " checked" : "",
        p.allow_ntp_offline ? " checked" : "");
    httpd_resp_send_chunk(req, buf, n);

    // --- The bridge's own clock ---
    //
    // Distinct from the NTP redirect above, which is what hotspot
    // clients are pointed at. This is the time the bridge itself keeps,
    // and it decides how schedule times are displayed.
    html_escape(p.ntp_server, e1, sizeof(e1));
    n = snprintf(buf, bs,
        "<fieldset><legend>Bridge clock</legend>"
        "<label>Time server</label>"
        "<input type=text name=\"ntp_srv\" value=\"%s\" "
        "placeholder=\"pool.ntp.org\">"
        "<small>Used by the bridge itself. Separate from the redirect "
        "for hotspot clients above.</small>"
        "<label>Time zone</label>"
        "<select name=\"tzname\">", e1);
    httpd_resp_send_chunk(req, buf, n);

    // The zone list, streamed: 120 options are more than one page buffer
    // holds.
    for (int i = 0; i < TZ_TABLE_COUNT; i++) {
        bool sel = strcmp(TZ_TABLE[i].name, p.tz_name) == 0;
        n = snprintf(buf, bs, "<option value=\"%s\"%s>%s</option>",
                     TZ_TABLE[i].name, sel ? " selected" : "",
                     TZ_TABLE[i].name);
        httpd_resp_send_chunk(req, buf, n);
    }

    n = snprintf(buf, bs,
        "</select>"
        "<small>Schedule times on the controller are local, so this "
        "decides how they are read.</small>"
        "<label>Summer time</label>"
        "<div class=swrow>"
        "<label class=sw><input type=checkbox id=dstsw name=\"dst_on\" "
        "value=\"1\"%s%s><span></span></label>"
        "<span id=dsttext>%s</span>"
        "</div>"
        "<div class=swrow><label class=sw><input type=checkbox id=dstauto "
        "name=\"dst_auto\" value=\"1\"%s onchange=\"dstToggle()\"><span></span></label><span>"
        "Follow the zone's own rules</span></div>"
        "<small>Following the rules is almost always right: the switch "
        "happens on the correct date without anything to remember. "
        "Unticking this pins the clock and the switch above decides "
        "which way.</small>"
        "<div class=swrow><label class=sw><input type=checkbox name=\"tz_push\" value=\"1\"%s><span></span></label><span>"
        "Apply this to every controller</span></div>"
        "<small>Sends the zone and daylight-saving choice to each "
        "controller as it connects, and puts it back if it is changed "
        "from the Spider Farmer app. Leave this off to set the zone per "
        "controller on the Control page.</small>"
        "<script>"
        // Greyed out while the rules decide: a switch that cannot take
        // effect should not look as though it can.
        "function dstToggle(){"
        "const a=document.getElementById('dstauto').checked;"
        "const s=document.getElementById('dstsw');"
        "s.disabled=a;"
        "document.getElementById('dsttext').textContent="
        "a?'decided by the zone rules':"
        "(s.checked?'forced on':'forced off');}"
        "document.getElementById('dstsw').addEventListener('change',"
        "dstToggle);"
        "</script>"
        "</fieldset>",
        p.dst_mode == SB_DST_SUMMER ? " checked" : "",
        p.dst_mode == SB_DST_AUTO   ? " disabled" : "",
        p.dst_mode == SB_DST_AUTO   ? "decided by the zone rules"
      : p.dst_mode == SB_DST_SUMMER ? "forced on"
                                    : "forced off",
        p.dst_mode == SB_DST_AUTO ? " checked" : "",
        p.tz_push ? " checked" : "");
    httpd_resp_send_chunk(req, buf, n);

    // --- Firmware updates from a URL ---
    html_escape(p.ota_url, e1, sizeof(e1));
    n = snprintf(buf, bs,
        "<fieldset><legend>Firmware updates</legend>"
        "<label>Update URL</label>"
        "<input type=text name=\"ota_url\" value=\"%s\" "
        "placeholder=\"http://host/firmware.json\">"
        "<small>Either a <code>.bin</code> image or a JSON file of the "
        "form <code>{\"version\":\"2.6.0\",\"url\":\"...bin\"}</code>. "
        "A JSON file lets the version be compared before anything is "
        "downloaded.</small>"
        "<div class=swrow><label class=sw><input type=checkbox name=\"ota_chk\" value=\"1\"%s><span></span></label><span>"
        "Check at startup</span></div>"
        "<div class=swrow><label class=sw><input type=checkbox name=\"ota_auto\" value=\"1\"%s><span></span></label><span>"
        "Install automatically when newer</span></div>"
        "<small>Without the second box a newer version is only "
        "reported on the Firmware page, where you can install it.</small>"
        "</fieldset>",
        e1,
        p.ota_check ? " checked" : "",
        p.ota_auto ? " checked" : "");
    httpd_resp_send_chunk(req, buf, n);

    // --- Web interface protection ---
    n = snprintf(buf, bs,
        "<fieldset><legend>Web interface protection</legend>"
        "<div class=swrow><label class=sw><input type=checkbox name=\"auth_on\" value=\"1\"%s><span></span></label><span>"
        "Require a password</span></div>"
        "<label>Password for user &quot;admin&quot;</label>"
        "<input type=password name=\"admin_pass\" placeholder=\"%s\">"
        "<small>Leave empty to keep the stored password. Unticking the "
        "box removes protection entirely.</small>"
        "</fieldset>"
        "<button type=\"submit\">Save and restart</button>"
        "</form>",
        p.admin_pass[0] ? " checked" : "",
        p.admin_pass[0] ? "saved" : "not set");
    httpd_resp_send_chunk(req, buf, n);

    // --- Backup, restore, reset ---
    //
    // Deliberately after the form closes. These act immediately rather
    // than on save, and nesting them inside the form would mean pressing
    // Enter in a text field could trigger one.
    n = snprintf(buf, bs,
        "<fieldset><legend>Backup</legend>"
        "<p><a href=\"/backup\"><button type=button>Download backup"
        "</button></a></p>"
        "<small>A JSON file with every setting, <b>including the WiFi "
        "and broker passwords</b>. Keep it somewhere private.</small>"
        "<p><input type=file id=rst accept=\".json\"> "
        "<button type=button onclick=\"doRestore()\">Restore</button></p>"
        "<small>Applies a backup and restarts.</small>"
        "<div id=\"rmsg\" style=\"font-size:.82rem;color:#9aa0aa\"></div>"
        "</fieldset>"
        "<fieldset style=\"border-color:#7f1d1d\">"
        "<legend style=\"color:#fca5a5\">Danger zone</legend>"
        "<p><button type=button onclick=\"doReboot()\">Restart bridge"
        "</button></p>"
        "<small>Takes about 15 seconds.</small>"
        "<p><input type=text id=cfm placeholder=\"type RESET\" size=14> "
        "<button type=button style=\"border-color:#7f1d1d;color:#fca5a5\" "
        "onclick=\"doReset()\">Factory reset</button></p>"
        "<small>Clears the WiFi credentials, the broker settings, every "
        "device name and both access lists. The bridge comes back "
        "unconfigured on its own hotspot at 192.168.10.1.</small>"
        "<div id=\"dmsg\" style=\"font-size:.82rem;color:#fca5a5\"></div>"
        "</fieldset>");
    httpd_resp_send_chunk(req, buf, n);

    // The script in its own chunk: together with the markup above this
    // exceeded the page buffer.
    n = snprintf(buf, bs,
        "<script>"
        "async function doReboot(){"
        "if(!confirm('Restart the bridge now?'))return;"
        "await fetch('/reboot',{method:'POST'});"
        "document.getElementById('dmsg').textContent="
        "'Restarting, back in about 15 seconds.';}"
        "async function doReset(){"
        "const c=document.getElementById('cfm').value;"
        "if(c!=='RESET'){document.getElementById('dmsg').textContent="
        "'Type RESET in the box to confirm.';return;}"
        "if(!confirm('This erases every setting. Continue?'))return;"
        "const r=await fetch('/system/reset',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'confirm=RESET'});"
        "document.getElementById('dmsg').textContent=await r.text();}"
        "async function doRestore(){"
        "const f=document.getElementById('rst').files[0];"
        "const m=document.getElementById('rmsg');"
        "if(!f){m.textContent='Choose a backup file first.';return;}"
        "m.textContent='Uploading...';"
        "try{const r=await fetch('/restore',{method:'POST',"
        "body:await f.text()});m.textContent=await r.text();}"
        "catch(e){m.textContent='Upload failed.';}}"
        "</script></body></html>");
    httpd_resp_send_chunk(req, buf, n);

    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /log  — live view of the MQTT traffic
//
// Polls /log/data for entries newer than the highest sequence number it
// already has, so only new packets cross the wire.
// ---------------------------------------------------------------------------
static const char LOG_PAGE[] =
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>SpiderBridge MQTT log</title><style>"
    "*{box-sizing:border-box}"
    "body{font-family:system-ui,sans-serif;margin:0;padding:1rem;"
    "background:#13151a;color:#e6e6e6}"
    "h1{font-size:1.2rem;margin:0 0 .2rem}"
    "p.sub{margin:0 0 1rem;color:#9aa0aa;font-size:.85rem}"
    ".bar{display:flex;gap:.5rem;flex-wrap:wrap;align-items:center;"
    "margin-bottom:.75rem;font-size:.85rem}"
    ".bar button{padding:.4rem .8rem;border:1px solid #333845;"
    "border-radius:.35rem;background:#1a1d24;color:#c3c8d0;cursor:pointer;"
    "font-size:.85rem}"
    ".bar button.on{background:#8ab4f8;color:#13151a;border-color:#8ab4f8;"
    "font-weight:600}"
    ".bar a{color:#8ab4f8;text-decoration:none;margin-left:auto}"
    "#log{font-family:ui-monospace,Consolas,monospace;font-size:.75rem;"
    "line-height:1.45;background:#0f1116;border:1px solid #2c303a;"
    "border-radius:.5rem;padding:.6rem;height:74vh;overflow-y:auto}"
    ".e{padding:.15rem 0;border-bottom:1px solid #1a1d24;"
    "white-space:pre-wrap;word-break:break-all}"
    ".t{color:#6b7280}"
    ".tag{display:inline-block;min-width:4.2rem;font-weight:600}"
    ".up .tag{color:#4ade80}.down .tag{color:#60a5fa}"
    ".haout .tag{color:#c084fc}.hain .tag{color:#fbbf24}"
    ".ty{color:#7d848f}.top{color:#e6e6e6}.pl{color:#9aa0aa}"
    ".empty{color:#6b7280;padding:1rem;text-align:center}"
    "#send{background:#1a1d24;border:1px solid #2c303a;border-radius:.5rem;"
    "padding:.7rem;margin-bottom:.75rem}"
    "#send summary{cursor:pointer;color:#8ab4f8;font-size:.85rem;"
    "font-weight:600}"
    "#send .r{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap;"
    "margin-top:.6rem;font-size:.82rem}"
    "#send label{color:#9aa0aa;min-width:4rem}"
    "#send select,#send input{padding:.35rem .5rem;background:#0f1116;"
    "color:#e6e6e6;border:1px solid #333845;border-radius:.3rem;"
    "font-size:.82rem;font-family:ui-monospace,monospace}"
    "#send textarea{width:100%;min-height:4.5rem;padding:.45rem;"
    "background:#0f1116;color:#e6e6e6;border:1px solid #333845;"
    "border-radius:.3rem;font-family:ui-monospace,monospace;font-size:.78rem;"
    "margin-top:.4rem;resize:vertical}"
    "#send button.go{background:#8ab4f8;color:#13151a;border-color:#8ab4f8;"
    "font-weight:600;padding:.4rem 1rem;border-radius:.3rem;cursor:pointer;"
    "border:1px solid #8ab4f8;font-size:.82rem}"
    "#send .hint{color:#6b7280;font-size:.75rem;margin-top:.4rem;"
    "line-height:1.5}"
    "#send .ex{color:#8ab4f8;cursor:pointer;text-decoration:underline;"
    "margin-right:.7rem}"
    "#sres{font-size:.8rem;margin-top:.5rem;min-height:1.2rem}"
    "#sres.ok{color:#4ade80}#sres.err{color:#f87171}";

// Closes the <style> block and opens <body>, and nothing more — the
// nav bar is written immediately after this, before any page content.
static const char LOG_BODY_OPEN[] = "</style></head><body>";

static const char LOG_PAGE_BODY[] =
    "<h1>Live MQTT log</h1>"
    "<p class=\"sub\">Controller traffic and what is forwarded to "
    "Home Assistant. Recorded only while this page is open and kept in "
    "your browser, not on the bridge; everything sent to Home Assistant "
    "also stays readable on the MQTT broker.</p>"
    "<div class=\"bar\">"
    "<button id=\"bAll\" class=\"on\" onclick=\"setF('all')\">All</button>"
    "<button id=\"bRaw\" onclick=\"setF('raw')\">Controller only</button>"
    "<button id=\"bHa\" onclick=\"setF('ha')\">Home Assistant only</button>"
    "<button id=\"bP\" onclick=\"tog()\">Pause</button>"
    "<button onclick=\"clearLog()\">Clear</button>"
    "<button onclick=\"saveLog()\">Download</button>"
    "</div>"
    "<details id=\"send\">"
    "<summary>Send a command</summary>"
    "<div class=\"r\">"
    "<label>Device</label><select id=\"sdev\"></select>"
    "<label>Mode</label>"
    "<select id=\"smode\" onchange=\"modeChanged()\">"
    "<option value=\"method\">Method (envelope added)</option>"
    "<option value=\"raw\">Raw JSON (sent verbatim)</option>"
    "<option value=\"ha\">Home Assistant field</option>"
    "</select>"
    "</div>"
    "<div class=\"r\" id=\"mrow\">"
    "<label id=\"mlbl\">Method</label>"
    "<input id=\"smeth\" style=\"flex:1;min-width:12rem\" "
    "placeholder=\"getConfigField\">"
    "</div>"
    "<textarea id=\"spay\" spellcheck=\"false\" "
    "placeholder='{\"keyPath\":[\"device\",\"humidifier\"]}'></textarea>"
    "<div class=\"r\">"
    "<button class=\"go\" onclick=\"sendCmd()\">Send</button>"
    "<span id=\"sres\"></span>"
    "</div>"
    "<div class=\"hint\" id=\"shint\"></div>"
    "</details>"
    "<div id=\"log\"><div class=\"empty\">Waiting for traffic&hellip;</div></div>"
    "<script>"
    // --- Manual sending ---
    "const EX={"
    "method:[['Read humidifier config','getConfigField',"
    "'{\"keyPath\":[\"device\",\"humidifier\"]}'],"
    "['Read fan config','getConfigField','{\"keyPath\":[\"device\",\"fan\"]}'],"
    "['Read full status','getDevSta',''],"
    "['Read alarm state','getConfigField',"
    "'{\"keyPath\":[\"device\",\"alarm\"]}']],"
    "raw:[['getDevSta','','{\"method\":\"getDevSta\",\"pid\":\"MAC\"}']],"
    "ha:[['Fan speed 5','fan/percentage','5'],"
    "['Light on','light','{\"state\":\"ON\"}'],"
    "['Humidifier on','humidifier','ON']]};"
    "function modeChanged(){"
    "const m=document.getElementById('smode').value;"
    "const lbl=document.getElementById('mlbl');"
    "const mi=document.getElementById('smeth');"
    "const pay=document.getElementById('spay');"
    "document.getElementById('mrow').style.display=m==='raw'?'none':'flex';"
    "if(m==='method'){lbl.textContent='Method';"
    "mi.placeholder='getConfigField';"
    "pay.placeholder='{\"keyPath\":[\"device\",\"humidifier\"]}';}"
    "else if(m==='ha'){lbl.textContent='Field';"
    "mi.placeholder='fan/percentage';pay.placeholder='5';}"
    "else{pay.placeholder='{\"method\":\"getDevSta\",\"pid\":\"...\"}';}"
    "let h='';"
    "if(m==='method')h='The pid, uid and message id are filled in for you. "
    "Leave the body empty when the method takes no parameters.<br>';"
    "else if(m==='raw')h='Sent byte for byte, nothing added. Use this when "
    "the envelope itself needs to be controlled.<br>';"
    "else h='Translated exactly like a Home Assistant command, so this "
    "exercises the real command path.<br>';"
    "h+='Examples: ';"
    "for(const[n,a,b]of EX[m])"
    "h+='<span class=ex onclick=\"ex(\\''+encodeURIComponent(a)+'\\',\\''+"
    "encodeURIComponent(b)+'\\')\">'+n+'</span>';"
    "document.getElementById('shint').innerHTML=h;}"
    "function ex(a,b){"
    "document.getElementById('smeth').value=decodeURIComponent(a);"
    "document.getElementById('spay').value=decodeURIComponent(b);}"
    "async function loadDevs(){"
    "try{const r=await fetch('/control/data');const j=await r.json();"
    "const s=document.getElementById('sdev');s.innerHTML='';"
    "for(const d of (j.devices||[])){"
    "const o=document.createElement('option');o.value=d.mac;"
    "o.textContent=d.name+(d.online?'':' (offline)');s.appendChild(o);}"
    "if(!s.options.length){const o=document.createElement('option');"
    "o.textContent='none connected';s.appendChild(o);}}catch(e){}}"
    "async function sendCmd(){"
    "const res=document.getElementById('sres');"
    "res.className='';res.textContent='Sending...';"
    "const body=new URLSearchParams({"
    "mode:document.getElementById('smode').value,"
    "d:document.getElementById('sdev').value||'',"
    "method:document.getElementById('smeth').value,"
    "p:document.getElementById('spay').value});"
    "try{const r=await fetch('/log/send',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:body.toString()});"
    "const t=await r.text();"
    "res.className=r.ok?'ok':'err';res.textContent=t;}"
    "catch(e){res.className='err';res.textContent='Connection failed';}}"
    "modeChanged();loadDevs();"
    "let seq=0,paused=false,filter='all';"
    "const N={0:['up','UP'],1:['down','DOWN'],2:['haout','HA OUT'],"
    "3:['hain','HA IN']};"
    "function setF(f){filter=f;"
    "for(const[i,v]of[['bAll','all'],['bRaw','raw'],['bHa','ha']])"
    "document.getElementById(i).className=(v===f?'on':'');"
    "if(typeof redraw==='function')redraw();}"
    "function tog(){paused=!paused;const b=document.getElementById('bP');"
    "b.textContent=paused?'Resume':'Pause';b.className=paused?'on':'';}"
    "function show(e){if(filter==='raw')return e.d<2;"
    "if(filter==='ha')return e.d>=2;return true;}"
    "function esc(s){return s.replace(/[<>&]/g,c=>"
    "({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]));}"
    // Backs off when the bridge reports memory pressure.
    //
    // A fixed one-second poll is what turned a tight heap into a reboot:
    // every request failed, the browser retried immediately, and the
    // allocation churn never let the heap recover. Backing off gives it
    // room and the interval returns to normal on the next good reply.
    // The browser is the archive: the bridge only hands entries over while
    // this page polls and forgets them once fetched. Kept in sessionStorage
    // so a reload or a hop to another page does not lose the history.
    "const KEEP=2000;"
    "let H=[];try{H=JSON.parse(sessionStorage.getItem('sblog')||'[]');}catch(e){H=[];}"
    "let lastSave=0;"
    "function persist(){const n=Date.now();if(n-lastSave<2000)return;lastSave=n;"
    "try{sessionStorage.setItem('sblog',JSON.stringify(H.slice(-KEEP)));}catch(e){}}"
    "function line(e){"
    "if(e.gap)return '<div class=e><span class=\"t\">\\u2026 '+e.gap+"
    "' entries not shown (bridge buffer overrun)</span></div>';"
    "const[c,l]=N[e.d]||['','?'];"
    "return '<div class=\"e '+c+'\"><span class=\"t\">'+"
    "new Date(e.w).toLocaleTimeString()+'</span> '+"
    "'<span class=\"tag\">'+l+'</span> '+"
    "'<span class=\"ty\">'+e.t+'</span> '+"
    "'<span class=\"top\">'+esc(e.o||'')+'</span>'+"
    "(e.p?'\\n    <span class=\"pl\">'+esc(e.p)+"
    "(e.c?' \\u2026(+'+(e.l-e.p.length)+'B)':'')+'</span>':'')+'</div>';}"
    "function redraw(){const box=document.getElementById('log');"
    "const v=H.filter(e=>e.gap||show(e)).slice(-500);"
    "box.innerHTML=v.length?v.map(line).join(''):"
    "'<div class=\"empty\">Waiting for traffic&hellip;</div>';"
    "box.scrollTop=box.scrollHeight;}"
    "function clearLog(){H=[];sessionStorage.removeItem('sblog');redraw();}"
    "function saveLog(){"
    "const t=H.map(e=>e.gap?'... '+e.gap+' entries not shown':"
    "new Date(e.w).toISOString()+' '+(N[e.d]||['','?'])[1]+' '+e.t+' '+"
    "(e.o||'')+(e.p?'\\n    '+e.p:'')).join('\\n');"
    "const a=document.createElement('a');"
    "a.href=URL.createObjectURL(new Blob([t],{type:'text/plain'}));"
    "a.download='spiderbridge-mqtt-log.txt';a.click();}"
    "let wait=500;"
    "async function poll(){"
    "if(paused){setTimeout(poll,1000);return;}"
    "try{const r=await fetch('/log/data?since='+seq);const j=await r.json();"
    "if(j.busy){wait=Math.min(wait*2,8000);setTimeout(poll,wait);return;}"
    "wait=500;"
    "if(j.e.length){"
    "const box=document.getElementById('log');"
    "const em=box.querySelector('.empty');if(em)em.remove();"
    "const atEnd=box.scrollHeight-box.scrollTop-box.clientHeight<60;"
    "let html='';"
    "for(const e of j.e){"
    // The bridge's numbering restarts after a reboot; a smaller number
    // than the last one seen is a fresh start, not a gap.
    "if(seq&&e.s>seq+1){const g={gap:e.s-seq-1};H.push(g);html+=line(g);}"
    "seq=e.s;e.w=Date.now();H.push(e);"
    "if(show(e))html+=line(e);}"
    "box.insertAdjacentHTML('beforeend',html);"
    "while(box.children.length>500)box.removeChild(box.firstChild);"
    "if(H.length>KEEP)H=H.slice(-KEEP);"
    "persist();"
    "if(atEnd)box.scrollTop=box.scrollHeight;}"
    "if(j.n<seq)seq=0;"
    "}catch(err){wait=Math.min(wait*2,8000);}"
    "setTimeout(poll,wait);}"
    "redraw();poll();"
    "</script></body></html>";

static esp_err_t log_page_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, LOG_PAGE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NAV_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, LOG_BODY_OPEN, HTTPD_RESP_USE_STRLEN);
    nav_send(req, NAV_LOG);
    httpd_resp_send_chunk(req, LOG_PAGE_BODY, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Escapes a string for embedding in JSON.
static void json_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 7 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
            case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
            case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
            default:
                if (c < 0x20) {
                    o += snprintf(dst + o, dst_sz - o, "\\u%04x", c);
                } else {
                    dst[o++] = (char)c;
                }
        }
    }
    dst[o] = '\0';
}

// ---------------------------------------------------------------------------
// GET /log/data?since=<seq>  — JSON feed for the live view
//
// Short field names keep each entry small; this runs on a device with a
// 1440-byte MSS and the whole response is streamed in chunks.
// ---------------------------------------------------------------------------
static esp_err_t log_data_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    uint32_t since = 0;
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[24];
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, NULL, 10);
        }
    }

    // A viewer is open: keep recording (or start, on the first poll).
    live_log_viewer_poll();

    // One entry at a time.
    //
    // This used to fetch a batch of 32 into a single array. Once the
    // payload limit was raised to 480 bytes that array became a 16 KB
    // contiguous allocation, requested once per second by the live view.
    // With two TLS sessions the heap sits near 49 KB and is fragmented,
    // so the request failed, and the browser kept retrying: a steady
    // stream of "httpd_resp_send_err: 500 - Out of memory" that ended in
    // the device rebooting.
    //
    // Streaming entry by entry needs one entry plus one chunk buffer —
    // about 1.5 KB instead of 16 KB — and the response is identical.
    // Up to the whole buffer per request, so a burst of state updates
    // between two polls is delivered in one go rather than over several.
    const int batch = SB_LOG_MAX_RECORDS;

    log_entry_t *entry = malloc(sizeof(log_entry_t));
    // Small fixed buffers: the header goes out in one chunk, the payload
    // is escaped and sent in pieces of 256 source bytes, so a 2 KB payload
    // needs no 4 KB escape buffer.
    const size_t chunk_sz = 640;
    char *chunk = malloc(chunk_sz);
    char *esc_topic = malloc(200);
    char *esc_payload = malloc(256 * 6 + 8);

    if (!entry || !chunk || !esc_topic || !esc_payload) {
        free(entry); free(chunk); free(esc_topic); free(esc_payload);
        // 503 rather than 500: this is transient memory pressure, not a
        // broken request, and the distinction matters when reading logs.
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"e\":[],\"busy\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"e\":[", HTTPD_RESP_USE_STRLEN);

    uint32_t cursor = since;
    int n = 0;
    for (int i = 0; i < batch; i++) {
        if (!live_log_fetch_one(cursor, entry)) break;
        cursor = entry->seq;

        json_escape(entry->topic, esc_topic, 200);

        int len = snprintf(chunk, chunk_sz,
            "%s{\"s\":%u,\"m\":%u,\"d\":%u,\"t\":\"%s\",\"o\":\"%s\",\"p\":\"",
            n ? "," : "",
            (unsigned)entry->seq, (unsigned)entry->ms, (unsigned)entry->dir,
            live_log_packet_name(entry->packet_type), esc_topic);
        if (len <= 0 || len >= (int)chunk_sz) continue;
        httpd_resp_send_chunk(req, chunk, len);

        // The payload, escaped piece by piece.
        size_t plen = strlen(entry->payload);
        for (size_t off = 0; off < plen; off += 256) {
            char piece[257];
            size_t k = plen - off < 256 ? plen - off : 256;
            memcpy(piece, entry->payload + off, k);
            piece[k] = '\0';
            json_escape(piece, esc_payload, 256 * 6 + 8);
            httpd_resp_send_chunk(req, esc_payload, HTTPD_RESP_USE_STRLEN);
        }

        len = snprintf(chunk, chunk_sz, "\",\"l\":%u,\"c\":%s}",
                       (unsigned)entry->payload_len,
                       entry->truncated ? "true" : "false");
        httpd_resp_send_chunk(req, chunk, len);
        n++;
    }

    int len = snprintf(chunk, chunk_sz, "],\"n\":%u}",
                       (unsigned)live_log_latest_seq());
    httpd_resp_send_chunk(req, chunk, len);
    httpd_resp_send_chunk(req, NULL, 0);

    free(entry); free(chunk); free(esc_topic); free(esc_payload);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /save
// ---------------------------------------------------------------------------

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    // Room for a pasted CA certificate.
    //
    // A PEM certificate is around 2 KB on its own, and URL-encoding the
    // newlines inflates it further — so the old 2048-byte cap would have
    // rejected the save outright, with "Invalid form size" as the only
    // clue. 10 KB covers two certificates (broker and syslog server) plus
    // every other field.
    if (req->content_len <= 0 || req->content_len > 10240) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form size");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    // Start from stored values so blank password fields keep the saved
    // secret instead of wiping it.
    sb_prov_cfg_t p;
    sb_prov_load(&p);

    // Snapshot of the fields that genuinely need a restart, taken before
    // the form overwrites them. Everything else is applied live further
    // down, so saving a switch no longer costs a reboot and the
    // controller's session survives it.
    char was_sta_ssid[sizeof(p.sta_ssid)];
    char was_sta_pass[sizeof(p.sta_pass)];
    char was_ap_ssid[sizeof(p.ap_ssid)];
    char was_ap_pass[sizeof(p.ap_pass)];
    char was_ap_ip[sizeof(p.ap_ip)];
    char was_static_ip[sizeof(p.static_ip)];
    char was_static_gw[sizeof(p.static_gw)];
    char was_static_mask[sizeof(p.static_mask)];
    char was_mqtt_uri[sizeof(p.ha_mqtt_uri)];
    char was_mqtt_user[sizeof(p.ha_mqtt_user)];
    char was_mqtt_pass[sizeof(p.ha_mqtt_pass)];
    char was_device_id[sizeof(p.device_id)];
    bool was_tls_insecure = p.mqtt_tls_insecure;
    strcpy(was_sta_ssid, p.sta_ssid);
    strcpy(was_sta_pass, p.sta_pass);
    strcpy(was_ap_ssid, p.ap_ssid);
    strcpy(was_ap_pass, p.ap_pass);
    strcpy(was_ap_ip, p.ap_ip);
    int was_ap_prefix = p.ap_prefix;
    bool was_ap_hidden = p.ap_hidden;
    strcpy(was_static_ip, p.static_ip);
    strcpy(was_static_gw, p.static_gw);
    strcpy(was_static_mask, p.static_mask);
    strcpy(was_mqtt_uri, p.ha_mqtt_uri);
    strcpy(was_mqtt_user, p.ha_mqtt_user);
    strcpy(was_mqtt_pass, p.ha_mqtt_pass);
    strcpy(was_device_id, p.device_id);

    char tmp[256];

    if (form_get(body, "sta_ssid", tmp, sizeof(tmp))) {
        strncpy(p.sta_ssid, tmp, sizeof(p.sta_ssid) - 1);
        p.sta_ssid[sizeof(p.sta_ssid) - 1] = '\0';
    }
    if (form_get(body, "sta_pass", tmp, sizeof(tmp)) && tmp[0] != '\0') {
        strncpy(p.sta_pass, tmp, sizeof(p.sta_pass) - 1);
        p.sta_pass[sizeof(p.sta_pass) - 1] = '\0';
    }
    if (form_get(body, "ap_ssid", tmp, sizeof(tmp)) && tmp[0] != '\0') {
        strncpy(p.ap_ssid, tmp, sizeof(p.ap_ssid) - 1);
        p.ap_ssid[sizeof(p.ap_ssid) - 1] = '\0';
    }
    if (form_get(body, "ap_pass", tmp, sizeof(tmp)) && tmp[0] != '\0') {
        strncpy(p.ap_pass, tmp, sizeof(p.ap_pass) - 1);
        p.ap_pass[sizeof(p.ap_pass) - 1] = '\0';
    }
    const char *subnet_err = NULL;
    if (form_get(body, "ap_subnet", tmp, sizeof(tmp)) && tmp[0] != '\0') {
        sb_prov_cfg_t trial = p;
        if (!sb_prov_set_ap_subnet(&trial, tmp)) {
            subnet_err = "Hotspot subnet not saved: use a.b.c.d/len with len 8 to 30.";
        } else {
            // Refuse a hotspot that overlaps the home network the bridge is
            // on right now: replies would leave through the wrong side.
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t si = {0};
            esp_netif_ip_info_t ai = {0};
            char mask[16];
            sb_prov_ap_netmask_str(&trial, mask, sizeof(mask));
            esp_netif_str_to_ip4(trial.ap_ip, &ai.ip);
            esp_netif_str_to_ip4(mask, &ai.netmask);
            if (sta && esp_netif_get_ip_info(sta, &si) == ESP_OK && si.ip.addr) {
                uint32_t m = ai.netmask.addr & si.netmask.addr;   // the wider of both
                if ((si.ip.addr & m) == (ai.ip.addr & m))
                    subnet_err = "Hotspot subnet not saved: it overlaps the home network.";
            }
            if (!subnet_err) { strcpy(p.ap_ip, trial.ap_ip); p.ap_prefix = trial.ap_prefix; }
        }
        if (subnet_err) ESP_LOGW(TAG, "%s (%s)", subnet_err, tmp);
    }
    if (form_get(body, "ha_uri", tmp, sizeof(tmp))) {
        strncpy(p.ha_mqtt_uri, tmp, sizeof(p.ha_mqtt_uri) - 1);
        p.ha_mqtt_uri[sizeof(p.ha_mqtt_uri) - 1] = '\0';
    }
    if (form_get(body, "ha_user", tmp, sizeof(tmp))) {
        strncpy(p.ha_mqtt_user, tmp, sizeof(p.ha_mqtt_user) - 1);
        p.ha_mqtt_user[sizeof(p.ha_mqtt_user) - 1] = '\0';
    }
    if (form_get(body, "ha_pass", tmp, sizeof(tmp)) && tmp[0] != '\0') {
        strncpy(p.ha_mqtt_pass, tmp, sizeof(p.ha_mqtt_pass) - 1);
        p.ha_mqtt_pass[sizeof(p.ha_mqtt_pass) - 1] = '\0';
    }
    if (form_get(body, "device_id", tmp, sizeof(tmp)) && tmp[0] != '\0') {
        strncpy(p.device_id, tmp, sizeof(p.device_id) - 1);
        p.device_id[sizeof(p.device_id) - 1] = '\0';
    }

    // Applied live (renames the device in Home Assistant) rather than by
    // restart; sb_prov_save() below writes the same value again.
    // Remote syslog, applied live: no restart needed to start or stop it.
    // Applied further down, after the CA certificate has been stored, so a
    // newly pasted certificate is used by the very first connection.
    bool slog_seen = false;
    if (form_get(body, "slog_host", tmp, sizeof(tmp))) {
        slog_seen = true;
        strncpy(p.syslog_host, tmp, sizeof(p.syslog_host) - 1);
        p.syslog_host[sizeof(p.syslog_host) - 1] = '\0';
        if (form_get(body, "slog_proto", tmp, sizeof(tmp))) {
            int pr = atoi(tmp);
            p.syslog_proto = (pr >= 0 && pr <= 2) ? pr : 0;
        }
        if (form_get(body, "slog_port", tmp, sizeof(tmp))) {
            int sp = atoi(tmp);
            p.syslog_port = (sp > 0 && sp < 65536) ? sp
                          : (p.syslog_proto == 2 ? 6514 : 514);
        }
        if (form_get(body, "slog_mqtt", tmp, sizeof(tmp))) {
            int m = atoi(tmp);
            p.syslog_mqtt = (m >= 0 && m <= 2) ? m : 0;
        }
        char *ca = malloc(2048);
        if (ca) {
            if (form_get(body, "slog_ca", ca, 2048)) {
                char *s = ca;
                while (*s == ' ' || *s == '\r' || *s == '\n') s++;
                size_t len = strlen(s);
                while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\r' ||
                                   s[len-1] == '\n')) s[--len] = '\0';
                if (strcmp(s, "-") == 0) {
                    sb_prov_syslog_ca_save(NULL);
                } else if (s[0]) {
                    sb_prov_syslog_ca_save(s);
                }
            }
            free(ca);
        }
    }

    // Empty field = the default name. The page shows the field empty when
    // the default is in use, so only a real change renames.
    if (form_get(body, "br_name", tmp, sizeof(tmp)) &&
        (tmp[0] ? strcmp(tmp, p.bridge_name) != 0 : !prov_bridge_name_is_default())) {
        ha_mqtt_rename_bridge(tmp);
        strncpy(p.bridge_name, prov_bridge_name(), sizeof(p.bridge_name) - 1);
        p.bridge_name[sizeof(p.bridge_name) - 1] = '\0';
    }

    if (form_get(body, "pubmode", tmp, sizeof(tmp))) {
        int mode = atoi(tmp);
        if (mode >= SB_PUB_HA && mode <= SB_PUB_BOTH) p.publish_mode = mode;
    }

    if (form_get(body, "dns_target", tmp, sizeof(tmp))) {
        strncpy(p.dns_target, tmp, sizeof(p.dns_target) - 1);
        p.dns_target[sizeof(p.dns_target) - 1] = '\0';
    }
    if (form_get(body, "ntp_target", tmp, sizeof(tmp))) {
        strncpy(p.ntp_target, tmp, sizeof(p.ntp_target) - 1);
        p.ntp_target[sizeof(p.ntp_target) - 1] = '\0';
    }
    if (form_get(body, "static_ip", tmp, sizeof(tmp))) {
        strncpy(p.static_ip, tmp, sizeof(p.static_ip) - 1);
        p.static_ip[sizeof(p.static_ip) - 1] = '\0';
    }
    if (form_get(body, "static_gw", tmp, sizeof(tmp))) {
        strncpy(p.static_gw, tmp, sizeof(p.static_gw) - 1);
        p.static_gw[sizeof(p.static_gw) - 1] = '\0';
    }
    if (form_get(body, "static_mask", tmp, sizeof(tmp)) && tmp[0] != '\0') {
        strncpy(p.static_mask, tmp, sizeof(p.static_mask) - 1);
        p.static_mask[sizeof(p.static_mask) - 1] = '\0';
    }

    // --- Bridge clock ---
    if (form_get(body, "ntp_srv", tmp, sizeof(tmp)) && tmp[0] != '\0') {
        strncpy(p.ntp_server, tmp, sizeof(p.ntp_server) - 1);
        p.ntp_server[sizeof(p.ntp_server) - 1] = '\0';
    }
    // The dropdown sends a zone name; the POSIX rules come from the
    // table. Storing both means the controller can be given the label it
    // expects alongside the rules that actually apply.
    if (form_get(body, "tzname", tmp, sizeof(tmp)) && tmp[0] != '\0') {
        const char *posix = tz_posix_for(tmp);
        if (posix) {
            strncpy(p.tz_name, tmp, sizeof(p.tz_name) - 1);
            p.tz_name[sizeof(p.tz_name) - 1] = '\0';
            strncpy(p.tz, posix, sizeof(p.tz) - 1);
            p.tz[sizeof(p.tz) - 1] = '\0';
        } else {
            ESP_LOGW(TAG, "Unknown time zone '%s' — keeping the current one",
                     tmp);
        }
    }
    // Two checkboxes rather than three radios: "follow the rules" wins,
    // and the switch only matters when it is unticked.
    //
    // A disabled checkbox is not submitted at all, so the switch is read
    // from the stored value when automatic is on — otherwise ticking
    // automatic would also silently clear the forced direction.
    //
    // Only for a whole-page submit: a partial POST carries neither box and
    // would otherwise force standard time.
    if (strstr(body, "form=full")) {
        bool dst_auto = form_get(body, "dst_auto", tmp, sizeof(tmp));
        bool dst_on   = form_get(body, "dst_on", tmp, sizeof(tmp));
        if (dst_auto) {
            p.dst_mode = SB_DST_AUTO;
        } else {
            p.dst_mode = dst_on ? SB_DST_SUMMER : SB_DST_STANDARD;
        }
    }

    // --- Firmware updates ---
    if (form_get(body, "ota_url", tmp, sizeof(tmp))) {
        strncpy(p.ota_url, tmp, sizeof(p.ota_url) - 1);
        p.ota_url[sizeof(p.ota_url) - 1] = '\0';
    }

    // --- Broker TLS ---
    //
    // An empty textarea keeps the stored certificate, matching how the
    // password fields behave. A single "-" is the explicit way to clear
    // it, since an empty field cannot mean both "keep" and "remove".
    {
        char *ca = malloc(2048);
        if (ca) {
            if (form_get(body, "mqtt_ca", ca, 2048)) {
                // Trim surrounding whitespace, which a pasted PEM
                // almost always carries.
                char *s = ca;
                while (*s == ' ' || *s == '\r' || *s == '\n') s++;
                size_t len = strlen(s);
                while (len && (s[len-1] == ' ' || s[len-1] == '\r' ||
                               s[len-1] == '\n')) s[--len] = '\0';

                if (strcmp(s, "-") == 0) {
                    sb_prov_ca_cert_save(NULL);
                    ESP_LOGI(TAG, "CA certificate removed");
                } else if (s[0]) {
                    sb_prov_ca_cert_save(s);
                    ESP_LOGI(TAG, "CA certificate stored (%u bytes)",
                             (unsigned)strlen(s));
                }
            }
            free(ca);
        }
    }

    // An unticked checkbox is simply absent from the form body, so its
    // presence is what decides the value -- but only for a submit of the
    // whole Settings page (form=full). A partial POST (a script, a test,
    // another page) carries none of these fields, and reading their absence
    // as "off" switched off cloud mirroring, NAT, the redirects, the clock
    // push and the web password all at once.
    bool full_form = form_get(body, "form", tmp, sizeof(tmp)) &&
                     strcmp(tmp, "full") == 0;
    if (full_form) {
        p.wan_open           = form_get(body, "wan", tmp, sizeof(tmp));
        p.cloud_forward      = form_get(body, "cloudfw", tmp, sizeof(tmp));
        p.dns_redirect       = form_get(body, "dns_rd", tmp, sizeof(tmp));
        p.ntp_redirect       = form_get(body, "ntp_rd", tmp, sizeof(tmp));
        p.allow_dns_offline  = form_get(body, "dns_ex", tmp, sizeof(tmp));
        p.allow_ntp_offline  = form_get(body, "ntp_ex", tmp, sizeof(tmp));
        p.mqtt_tls_insecure  = form_get(body, "mqtt_ins", tmp, sizeof(tmp));
        p.ota_check          = form_get(body, "ota_chk", tmp, sizeof(tmp));
        p.ota_auto           = form_get(body, "ota_auto", tmp, sizeof(tmp));
        p.tz_push            = form_get(body, "tz_push", tmp, sizeof(tmp));
        p.ap_hidden          = form_get(body, "ap_hidden", tmp, sizeof(tmp));
        if (slog_seen) {
            p.syslog_tls_insecure = form_get(body, "slog_ins", tmp, sizeof(tmp));
        }

        // Web interface protection. Unticking the box clears the password;
        // leaving the field empty while ticked keeps the stored one.
        bool auth_wanted = form_get(body, "auth_on", tmp, sizeof(tmp));
        if (!auth_wanted) {
            p.admin_pass[0] = '\0';
        } else if (form_get(body, "admin_pass", tmp, sizeof(tmp)) && tmp[0] != '\0') {
            strncpy(p.admin_pass, tmp, sizeof(p.admin_pass) - 1);
            p.admin_pass[sizeof(p.admin_pass) - 1] = '\0';
        }
    } else {
        // Partial update: a checkbox is changed only when it is named
        // explicitly with a value, e.g. "cloudfw=1" or "cloudfw=0".
        struct { const char *key; bool *dst; } CB[] = {
            { "wan",      &p.wan_open },
            { "cloudfw",  &p.cloud_forward },
            { "dns_rd",   &p.dns_redirect },
            { "ntp_rd",   &p.ntp_redirect },
            { "dns_ex",   &p.allow_dns_offline },
            { "ntp_ex",   &p.allow_ntp_offline },
            { "mqtt_ins", &p.mqtt_tls_insecure },
            { "ota_chk",  &p.ota_check },
            { "ota_auto", &p.ota_auto },
            { "tz_push",  &p.tz_push },
            { "slog_ins", &p.syslog_tls_insecure },
            { "ap_hidden", &p.ap_hidden },
        };
        for (size_t i = 0; i < sizeof(CB) / sizeof(CB[0]); i++) {
            if (form_get(body, CB[i].key, tmp, sizeof(tmp))) {
                *CB[i].dst = !(tmp[0] == '0' || tmp[0] == '\0');
            }
        }
    }

    free(body);

    sb_prov_save(&p);

    if (slog_seen) {
        syslog_fwd_configure(p.syslog_host, p.syslog_port, p.syslog_proto,
                             p.syslog_tls_insecure);
        syslog_fwd_set_mqtt(p.syslog_mqtt);
    }

    // Only restart for the things that genuinely cannot change at
    // runtime.
    //
    // Everything here used to restart unconditionally, which meant
    // flipping one switch dropped the controller's TLS session and cost
    // the better part of a minute. Radio credentials and interface
    // addresses do need the stack rebuilt; a redirect target or a
    // feature toggle does not.
    // Named one at a time so the log says which field forced a restart.
    // A silent "needs_restart" is impossible to debug when it fires for
    // a field the user did not knowingly touch.
    const char *restart_reason = NULL;
    if (strcmp(was_sta_ssid, p.sta_ssid) != 0)         restart_reason = "WiFi SSID";
    else if (strcmp(was_sta_pass, p.sta_pass) != 0)    restart_reason = "WiFi password";
    else if (strcmp(was_ap_ssid, p.ap_ssid) != 0)      restart_reason = "hotspot SSID";
    else if (strcmp(was_ap_pass, p.ap_pass) != 0)      restart_reason = "hotspot password";
    else if (strcmp(was_ap_ip, p.ap_ip) != 0 ||
             was_ap_prefix != p.ap_prefix)              restart_reason = "hotspot subnet";
    else if (was_ap_hidden != p.ap_hidden)             restart_reason = "hidden hotspot";
    else if (strcmp(was_static_ip, p.static_ip) != 0)  restart_reason = "static IP";
    else if (strcmp(was_static_gw, p.static_gw) != 0)  restart_reason = "gateway";
    else if (strcmp(was_static_mask, p.static_mask) != 0) restart_reason = "netmask";
    // The MQTT client is created once with these, so changing any of
    // them means building a new client — a restart is the honest way
    // to do that.
    else if (strcmp(was_mqtt_uri, p.ha_mqtt_uri) != 0)   restart_reason = "broker URI";
    else if (strcmp(was_mqtt_user, p.ha_mqtt_user) != 0) restart_reason = "broker user";
    else if (strcmp(was_mqtt_pass, p.ha_mqtt_pass) != 0) restart_reason = "broker password";
    else if (strcmp(was_device_id, p.device_id) != 0)    restart_reason = "device id";
    else if (was_tls_insecure != p.mqtt_tls_insecure)    restart_reason = "broker TLS mode";

    bool needs_restart = restart_reason != NULL;
    if (needs_restart) {
        ESP_LOGI(TAG, "Restart needed: %s changed", restart_reason);
    }

    if (!needs_restart) {
        // Apply the live-settable settings right now. Each of these
        // already has a setter that takes effect immediately; they were
        // simply never called from this form.
        wan_gate_set(p.wan_open);
        wan_gate_set_cloud_forward(p.cloud_forward);
        dns_set_target(p.dns_target, p.dns_redirect);
        dns_set_ntp_target(p.ntp_target, p.ntp_redirect);
        dns_set_offline_exceptions(p.allow_dns_offline, p.allow_ntp_offline);
        time_sync_apply_tz();
        web_auth_reload();
        ha_mqtt_publish_bridge_state();
        if (p.tz_push) sf_push_timezone_all();

        ESP_LOGI(TAG, "Configuration saved and applied without restarting");

        httpd_resp_set_type(req, "text/html; charset=utf-8");
        char page[640];
        snprintf(page, sizeof(page),
            "<html><head><meta http-equiv=\"refresh\" content=\"%d;url=/\">"
            "</head><body style=\"font-family:system-ui;background:#13151a;"
            "color:#e6e6e6;padding:2rem\">"
            "<h1>Saved</h1><p>Applied immediately, no restart needed.</p>%s%s%s"
            "</body></html>", subnet_err ? 6 : 1,
            subnet_err ? "<p style=\"color:#f87171\">" : "", subnet_err ? subnet_err : "",
            subnet_err ? "</p>" : "");
        httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Configuration saved (STA SSID='%s', broker='%s') — "
                  "restarting, network settings changed",
             p.sta_ssid, p.ha_mqtt_uri);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char page[600];
    snprintf(page, sizeof(page), "<html><body style=\"font-family:system-ui;"
             "background:#13151a;color:#e6e6e6;padding:2rem\">"
             "<h1>Saved</h1><p>Restarting (%s changed)...</p>%s%s%s</body></html>",
             restart_reason,
             strcmp(restart_reason, "hotspot subnet") == 0
                 ? "<p>On the hotspot this page moves to <b>http://" : "",
             strcmp(restart_reason, "hotspot subnet") == 0 ? p.ap_ip : "",
             strcmp(restart_reason, "hotspot subnet") == 0
                 ? "/</b>. Connected controllers rejoin and get new addresses.</p>" : "");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Bluetooth GGS setup
//
// GET  /ble/data  scan results, each marked as already on the hotspot (with
//                 its IP) or only seen over Bluetooth.
// POST /ble/act   a=scan               start a scan
//                 a=connect&addr=..    send the hotspot's Wi-Fi to that GGS
// ---------------------------------------------------------------------------

// The hotspot client with this Wi-Fi MAC ("D0CF137A60B8"), if any.
// Returns 0 = not on the hotspot, 1 = associated but no address (the
// password does not match: the handshake fails before DHCP), 2 = connected
// with an address (copied to ip).
static int hotspot_client(const char *wifi_mac, char *ip, size_t ipsz)
{
    if (!wifi_mac || strlen(wifi_mac) != 12) return 0;
    mac_client_t cl[10];
    int n = mac_filter_list_clients(cl, 10);
    for (int i = 0; i < n; i++) {
        char hex[13];
        snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
                 cl[i].mac[0], cl[i].mac[1], cl[i].mac[2],
                 cl[i].mac[3], cl[i].mac[4], cl[i].mac[5]);
        if (strcmp(hex, wifi_mac) == 0) {
            if (!cl[i].ip) { ip[0] = '\0'; return 1; }
            snprintf(ip, ipsz, IPSTR, IP2STR((esp_ip4_addr_t *)&cl[i].ip));
            return 2;
        }
    }
    return 0;
}

// The reverse: Wi-Fi MAC "D0CF137A60B8" -> Bluetooth "D0:CF:13:7A:60:BA".
static bool ble_from_wifi_mac(const char *mac, char *out, size_t outsz)
{
    if (!mac || strlen(mac) != 12) return false;
    uint64_t m = 0;
    for (int i = 0; i < 12; i++) {
        char c = mac[i];
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        m = (m << 4) | (uint64_t)v;
    }
    m = (m + 2) & 0xFFFFFFFFFFFFull;
    snprintf(out, outsz, "%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)(m >> 40) & 0xFF, (unsigned)(m >> 32) & 0xFF, (unsigned)(m >> 24) & 0xFF,
             (unsigned)(m >> 16) & 0xFF, (unsigned)(m >> 8) & 0xFF, (unsigned)m & 0xFF);
    return true;
}

// An ESP32's Bluetooth address is its Wi-Fi station MAC plus 2. Used when
// the advertisement carries no (or an unreadable) Wi-Fi MAC.
static bool wifi_mac_from_ble(const char *addr, char *out, size_t outsz)
{
    unsigned v[6];
    if (sscanf(addr, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    uint64_t m = 0;
    for (int i = 0; i < 6; i++) m = (m << 8) | (v[i] & 0xFF);
    m = (m - 2) & 0xFFFFFFFFFFFFull;
    snprintf(out, outsz, "%02X%02X%02X%02X%02X%02X",
             (unsigned)(m >> 40) & 0xFF, (unsigned)(m >> 32) & 0xFF, (unsigned)(m >> 24) & 0xFF,
             (unsigned)(m >> 16) & 0xFF, (unsigned)(m >> 8) & 0xFF, (unsigned)m & 0xFF);
    return true;
}

// --- Home network: scan and live connect ---
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    wifi_scan_ap_t *ap = calloc(20, sizeof(*ap));
    if (!ap) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory"); return ESP_FAIL; }
    int n = wifi_apsta_scan(ap, 20);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ap[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", ap[i].rssi);
        cJSON_AddNumberToObject(o, "ch", ap[i].channel);
        cJSON_AddBoolToObject(o, "open", ap[i].auth == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    free(ap);
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, out ? out : "[]", HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static struct {
    volatile bool running, ok;
    char ssid[64], pass[128];
    char msg[160];   // no quotes or backslashes: shown as JSON as-is
} s_wjob;

static void wifi_connect_task(void *arg)
{
    (void)arg;
    char msg[160];
    bool ok = wifi_apsta_connect_now(s_wjob.ssid, s_wjob.pass, 20000, msg, sizeof(msg));
    if (ok) {
        // Kept only once it works, so a typo cannot lock the bridge out.
        sb_prov_cfg_t *p = malloc(sizeof(*p));
        if (p) {
            sb_prov_load(p);
            strncpy(p->sta_ssid, s_wjob.ssid, sizeof(p->sta_ssid) - 1);
            p->sta_ssid[sizeof(p->sta_ssid) - 1] = '\0';
            strncpy(p->sta_pass, s_wjob.pass, sizeof(p->sta_pass) - 1);
            p->sta_pass[sizeof(p->sta_pass) - 1] = '\0';
            sb_prov_save(p);
            memset(p, 0, sizeof(*p));
            free(p);
        }
        ESP_LOGI(TAG, "Home network \"%s\" stored", s_wjob.ssid);
        strncat(msg, " -- saved", sizeof(msg) - strlen(msg) - 1);
    }
    for (char *c = msg; *c; c++) if (*c == '"' || *c == '\\') *c = '\'';
    memset(s_wjob.pass, 0, sizeof(s_wjob.pass));
    strncpy(s_wjob.msg, msg, sizeof(s_wjob.msg) - 1);
    s_wjob.ok = ok;
    s_wjob.running = false;
    vTaskDelete(NULL);
}

static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    char body[300];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed"); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';
    sb_prov_cfg_t *p = malloc(sizeof(*p));
    if (!p) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory"); return ESP_FAIL; }
    sb_prov_load(p);
    char ssid[64] = "", pass[128] = "";
    form_get(body, "ssid", ssid, sizeof(ssid));
    bool pass_given = form_get(body, "pass", pass, sizeof(pass)) && pass[0];
    // An empty password field means "the stored one", if it is the same network.
    if (!pass_given && strcmp(ssid, p->sta_ssid) == 0) strncpy(pass, p->sta_pass, sizeof(pass) - 1);

    free(p);
    // Runs on its own task: joining can take 20 s, and the web server has
    // a single task -- every other page used to hang for that long. The
    // page polls /wifi/status for the result.
    httpd_resp_set_type(req, "text/plain");
    if (s_wjob.running) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "A connection attempt is already running");
    } else {
        memset(&s_wjob, 0, sizeof(s_wjob));
        strncpy(s_wjob.ssid, ssid, sizeof(s_wjob.ssid) - 1);
        strncpy(s_wjob.pass, pass, sizeof(s_wjob.pass) - 1);
        s_wjob.running = true;
        snprintf(s_wjob.msg, sizeof(s_wjob.msg), "Connecting to \"%s\" ...", ssid);
        if (xTaskCreate(wifi_connect_task, "sb_wconn", 4096, NULL, 3, NULL) != pdPASS) {
            s_wjob.running = false;
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "Could not start the connection attempt");
        } else {
            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_sendstr(req, s_wjob.msg);
        }
    }
    memset(pass, 0, sizeof(pass));
    return ESP_OK;
}

static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    char out[220];
    snprintf(out, sizeof(out), "{\"running\":%s,\"ok\":%s,\"msg\":\"%s\"}",
             s_wjob.running ? "true" : "false", s_wjob.ok ? "true" : "false", s_wjob.msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

// The detailed log of the last Bluetooth job, streamed from flash.
static esp_err_t ble_log_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t total = ggs_ble_log_size();
    if (!total) {
        httpd_resp_sendstr(req, "No Bluetooth job has run yet.\n");
        return ESP_OK;
    }
    char *b = malloc(1024);
    if (!b) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory"); return ESP_FAIL; }
    for (size_t off = 0; off < total;) {
        size_t n = ggs_ble_log_read(off, b, 1024);
        if (!n) break;
        if (httpd_resp_send_chunk(req, b, n) != ESP_OK) break;
        off += n;
    }
    free(b);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ble_data_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    ggs_ble_status_t *st = malloc(sizeof(*st));
    if (!st) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory"); return ESP_FAIL; }
    ggs_ble_get_status(st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", st->pending);
    cJSON_AddBoolToObject(root, "scanned", st->scanned);
    cJSON_AddStringToObject(root, "busy", st->busy_addr);
    cJSON_AddStringToObject(root, "result", st->last_result);
    cJSON_AddStringToObject(root, "trace", st->trace);
    // The network a setup hands out, for the confirmation question.
    sb_prov_cfg_t *pc = malloc(sizeof(*pc));
    if (pc) {
        sb_prov_load(pc);
        cJSON_AddStringToObject(root, "ap_ssid", pc->ap_ssid);
        free(pc);
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "dev");
    for (int i = 0; i < st->count; i++) {
        const ggs_ble_dev_t *d = &st->dev[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "addr", d->addr);
        cJSON_AddStringToObject(o, "name", d->name);
        cJSON_AddNumberToObject(o, "rssi", d->rssi);
        // Which Wi-Fi client is this controller? The MAC from its
        // advertisement first, then the one derived from its Bluetooth
        // address -- whichever is actually on the hotspot or known.
        char cand[2][13] = { "", "" };
        if (strlen(d->wifi_mac) == 12) strcpy(cand[0], d->wifi_mac);
        wifi_mac_from_ble(d->addr, cand[1], sizeof(cand[1]));
        const char *mac = cand[0][0] ? cand[0] : cand[1];
        char ip[20] = "";
        int hs = 0;
        for (int c = 0; c < 2 && !hs; c++) {
            if (cand[c][0] && (hs = hotspot_client(cand[c], ip, sizeof(ip))) != 0) mac = cand[c];
        }
        bool on = hs == 2;
        device_registry_lock();
        device_entry_t *de = NULL;
        for (int c = 0; c < 2 && !de; c++) {
            if (!cand[c][0]) continue;
            de = device_registry_find(cand[c]);
            if (de && !hs) mac = cand[c];
        }
        if (de) {
            cJSON_AddStringToObject(o, "known_as", de->name);
            cJSON_AddBoolToObject(o, "session", de->online);
        }
        device_registry_unlock();
        cJSON_AddStringToObject(o, "wifi_mac", mac);
        if (d->has_flags) {
            // What the controller itself advertised at scan time.
            cJSON_AddNumberToObject(o, "pcode", d->pcode);
            cJSON_AddBoolToObject(o, "adv_active", d->flags & GGS_FLAG_ACTIVE);
            cJSON_AddBoolToObject(o, "adv_wifi", d->flags & GGS_FLAG_WIFI);
            cJSON_AddBoolToObject(o, "adv_cloud", d->flags & GGS_FLAG_MQTT);
        }
        cJSON_AddBoolToObject(o, "connected", on);
        cJSON_AddStringToObject(o, "hs", hs == 2 ? "ip" : hs == 1 ? "noip" : "off");
        if (on) cJSON_AddStringToObject(o, "ip", ip);
        cJSON_AddItemToArray(arr, o);
    }

    // Controllers the bridge knows that the scan did not see (a controller
    // paired with a phone does not advertise, or the scan was too short):
    // listed too, with their hotspot state. Selectable all the same -- the
    // Bluetooth address is the Wi-Fi MAC plus 2, so a send can still be
    // tried; it simply fails when the controller is not advertising.
    device_registry_lock();
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        device_entry_t *de = device_registry_at(i);
        if (!de) continue;
        bool seen = false;
        for (int k = 0; k < st->count && !seen; k++) {
            char a[13];
            if (strcasecmp(st->dev[k].wifi_mac, de->mac) == 0) seen = true;
            else if (wifi_mac_from_ble(st->dev[k].addr, a, sizeof(a)) && strcasecmp(a, de->mac) == 0) seen = true;
        }
        if (seen) continue;
        char mac[13], name[DEV_NAME_LEN], ip[20] = "";
        strncpy(mac, de->mac, sizeof(mac) - 1); mac[12] = '\0';
        strncpy(name, de->name, sizeof(name) - 1); name[sizeof(name) - 1] = '\0';
        bool sess = de->online;
        device_registry_unlock();
        int hs = hotspot_client(mac, ip, sizeof(ip));
        char baddr[18] = "";
        ble_from_wifi_mac(mac, baddr, sizeof(baddr));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "addr", baddr);
        cJSON_AddStringToObject(o, "name", "");
        cJSON_AddStringToObject(o, "known_as", name);
        cJSON_AddStringToObject(o, "wifi_mac", mac);
        cJSON_AddBoolToObject(o, "session", sess);
        cJSON_AddBoolToObject(o, "connected", hs == 2);
        cJSON_AddStringToObject(o, "hs", hs == 2 ? "ip" : hs == 1 ? "noip" : "off");
        cJSON_AddBoolToObject(o, "not_seen", true);
        if (hs == 2) cJSON_AddStringToObject(o, "ip", ip);
        cJSON_AddItemToArray(arr, o);
        device_registry_lock();
    }
    device_registry_unlock();
    free(st);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t ble_act_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    if (req->content_len <= 0 || req->content_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request size");
        return ESP_FAIL;
    }
    char body[257];
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';

    char a[16] = "", addr[20] = "", tmp[8] = "";
    form_get(body, "a", a, sizeof(a));
    form_get(body, "addr", addr, sizeof(addr));
    bool bind = form_get(body, "bind", tmp, sizeof(tmp)) && tmp[0] == '1';
    bool ok = false;
    const char *msg = "Unknown action";
    // All restart the bridge into a short Bluetooth boot (about 30 s for a
    // scan, under a minute for a setup) and back; the page reloads itself.
    if (strcmp(a, "scan") == 0) {
        ok = ggs_ble_request_scan(1500);
        msg = ok ? "Restarting into Bluetooth to scan -- back in about 30 seconds"
                 : "Bluetooth is busy";
    } else if ((strcmp(a, "connect") == 0 || strcmp(a, "unpair") == 0) && addr[0]) {
        // Only a controller the bridge knows: from the last scan, or a
        // registered controller by its derived Bluetooth address. Nothing
        // else ever gets the hotspot's credentials.
        bool listed = false;
        ggs_ble_status_t *st = malloc(sizeof(*st));
        if (st) {
            ggs_ble_get_status(st);
            for (int i = 0; i < st->count; i++)
                if (strcasecmp(st->dev[i].addr, addr) == 0) { listed = true; strcpy(addr, st->dev[i].addr); }
            free(st);
        }
        if (!listed) {
            device_registry_lock();
            for (int i = 0; i < SB_MAX_DEVICES && !listed; i++) {
                device_entry_t *de = device_registry_at(i);
                char d[18];
                if (de && ble_from_wifi_mac(de->mac, d, sizeof(d)) && strcasecmp(d, addr) == 0) {
                    listed = true;
                    strcpy(addr, d);
                }
            }
            device_registry_unlock();
        }
        if (!listed) {
            msg = "That controller is not known -- scan again";
        } else if (strcmp(a, "unpair") == 0) {
            ok = ggs_ble_request_unpair(addr, 1500);
            msg = ok ? "Restarting into Bluetooth to unpair the controller -- back in about a minute"
                     : "Bluetooth is busy";
        } else {
            ok = ggs_ble_request_provision(addr, bind, 1500);
            msg = ok ? (bind ? "Restarting into Bluetooth to send the Wi-Fi settings and bind the controller -- back in about a minute"
                             : "Restarting into Bluetooth to send the Wi-Fi settings -- back in about a minute")
                     : "Bluetooth is busy";
        }
    }
    httpd_resp_set_type(req, "text/plain");
    if (!ok) httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

bool config_portal_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    // Let the server close the least-recently-used socket when every slot
    // is taken, instead of refusing the new request.
    //
    // With the purge off, running out of sockets looks exactly like a
    // hang: no further request is answered until recv_wait_timeout and
    // send_wait_timeout release the stale connections, which is up to
    // ten seconds. That is what made the interface die and then come
    // back on its own with no restart in between -- a mutex deadlock
    // cannot resolve itself and a stack overflow reboots, so neither
    // explained what was actually seen.
    //
    // It is easy to reach with several tabs open: every one of them
    // polls the device every five seconds and holds its own socket.
    cfg.lru_purge_enable = true;
    // A few pages open at once on top of the LRU purge. Must leave room in
    // CONFIG_LWIP_MAX_SOCKETS (16) for the proxy (listener + 2 per
    // session), MQTT, DNS, NTP and syslog: with 10 here and 12 in total
    // httpd_start failed and port 80 stayed closed (1.13.0).
    cfg.max_open_sockets = 6;
    // Give a slow request time to finish rather than dropping it, since
    // publishing discovery briefly competes for the same CPU.
    // Receive: 10 s for a slow request body. Send: 5 s. On a weak Wi-Fi
    // link a client that stops reading kept the single server task inside
    // one send for the full 10 s, and every other page waited behind it
    // (seen in soaks as 10-24 s stalls while ping showed loss). A response
    // that cannot move in 5 s is abandoned; the page simply asks again.
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 5;

    // Header and URI limits are build-time settings in this IDF version,
    // not members of httpd_config_t — see CONFIG_HTTPD_MAX_REQ_HDR_LEN
    // and CONFIG_HTTPD_MAX_URI_LEN in sdkconfig.defaults. The default
    // 512-byte header block is smaller than what a browser sends once it
    // includes a Referer and the Sec-Fetch headers, which surfaces as
    // "Header fields are too long" and a form that will not save.

    // Quieten the HTTP server's own logging.
    //
    // A polling page closes connections as it navigates, and each one
    // produces "httpd_sock_err: error in recv : 104". It is routine and
    // handled, but at warning level it drowns the system log that the
    // status page exists to show.
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    // The firmware upload parses multi-kilobyte chunks and verifies the
    // image header on this stack, so it needs more room than the plain
    // configuration pages.
    cfg.stack_size = 8192;
    // Registering more handlers than this limit fails silently: the
    // route simply never responds. Sixteen routes are registered, so the
    // headroom is deliberate.
    // 27 are registered now (the 24 before this was already one short,
    // which silently dropped /network/act).
    cfg.max_uri_handlers = 40;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the HTTP server");
        return false;
    }

    httpd_uri_t root    = { .uri = "/",             .method = HTTP_GET,  .handler = root_get_handler };
    httpd_uri_t save    = { .uri = "/save",         .method = HTTP_POST, .handler = save_post_handler };
    httpd_uri_t logpage = { .uri = "/log",          .method = HTTP_GET,  .handler = log_page_handler };
    httpd_uri_t logdata = { .uri = "/log/data",     .method = HTTP_GET,  .handler = log_data_handler };
    httpd_uri_t logsend = { .uri = "/log/send",     .method = HTTP_POST, .handler = mqtt_send_handler };
    httpd_uri_t ctrl    = { .uri = "/control",      .method = HTTP_GET,  .handler = control_page_handler };
    httpd_uri_t ctrldat = { .uri = "/control/data", .method = HTTP_GET,  .handler = control_data_handler };
    httpd_uri_t ctrlset = { .uri = "/control/set",  .method = HTTP_POST, .handler = control_set_handler };
    httpd_uri_t ctrldev = { .uri = "/control/device", .method = HTTP_POST, .handler = control_device_handler };
    httpd_uri_t ctrlzon = { .uri = "/control/zones", .method = HTTP_GET,  .handler = control_zones_handler };
    httpd_uri_t ctrlpln = { .uri = "/control/plan",  .method = HTTP_POST, .handler = control_plan_handler };
    httpd_uri_t ctrltps = { .uri = "/control/templates", .method = HTTP_GET, .handler = control_templates_handler };
    httpd_uri_t ctrltpl = { .uri = "/control/template",  .method = HTTP_POST, .handler = control_template_handler };
    httpd_uri_t lplang  = { .uri = "/control/lplan",     .method = HTTP_GET,  .handler = control_lplan_get_handler };
    httpd_uri_t lplanp  = { .uri = "/control/lplan",     .method = HTTP_POST, .handler = control_lplan_post_handler };
    httpd_uri_t stpage  = { .uri = "/status",        .method = HTTP_GET,  .handler = status_page_handler };
    httpd_uri_t stdata  = { .uri = "/status/data",   .method = HTTP_GET,  .handler = status_data_handler };
    httpd_uri_t sldata  = { .uri = "/syslog/data",   .method = HTTP_GET,  .handler = syslog_data_handler };
    httpd_uri_t stdst   = { .uri = "/status/dst",    .method = HTTP_POST, .handler = status_dst_handler };
    httpd_uri_t otapage = { .uri = "/update",        .method = HTTP_GET,  .handler = ota_page_handler };
    httpd_uri_t otapost = { .uri = "/update",        .method = HTTP_POST, .handler = ota_post_handler };
    httpd_uri_t otachk  = { .uri = "/update/check",  .method = HTTP_POST, .handler = ota_check_handler };
    httpd_uri_t otarem  = { .uri = "/update/remote", .method = HTTP_POST, .handler = ota_remote_handler };
    httpd_uri_t rebootu = { .uri = "/reboot",        .method = HTTP_POST, .handler = reboot_handler };
    httpd_uri_t sysrst  = { .uri = "/system/reset",  .method = HTTP_POST, .handler = system_reset_handler };
    httpd_uri_t backupu = { .uri = "/backup",        .method = HTTP_GET,  .handler = backup_get_handler };
    httpd_uri_t restore = { .uri = "/restore",       .method = HTTP_POST, .handler = restore_post_handler };
    httpd_uri_t netpage = { .uri = "/network",       .method = HTTP_GET,  .handler = network_page_handler };
    httpd_uri_t netdata = { .uri = "/network/data",  .method = HTTP_GET,  .handler = network_data_handler };
    httpd_uri_t netact  = { .uri = "/network/act",   .method = HTTP_POST, .handler = network_act_handler };
    httpd_uri_t bledata = { .uri = "/ble/data",      .method = HTTP_GET,  .handler = ble_data_handler };
    httpd_uri_t bleact  = { .uri = "/ble/act",       .method = HTTP_POST, .handler = ble_act_handler };
    httpd_uri_t blelog  = { .uri = "/ble/log",       .method = HTTP_GET,  .handler = ble_log_handler };
    httpd_uri_t wscan   = { .uri = "/wifi/scan",     .method = HTTP_GET,  .handler = wifi_scan_handler };
    httpd_uri_t wconn   = { .uri = "/wifi/connect",  .method = HTTP_POST, .handler = wifi_connect_handler };
    httpd_uri_t wstat   = { .uri = "/wifi/status",   .method = HTTP_GET,  .handler = wifi_status_handler };
    httpd_uri_t ctrlver = { .uri = "/control/ver",   .method = HTTP_GET,  .handler = control_ver_handler };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &logpage);
    httpd_register_uri_handler(server, &logdata);
    httpd_register_uri_handler(server, &logsend);
    httpd_register_uri_handler(server, &ctrl);
    httpd_register_uri_handler(server, &ctrldat);
    httpd_register_uri_handler(server, &ctrlset);
    httpd_register_uri_handler(server, &ctrldev);
    httpd_register_uri_handler(server, &ctrlzon);
    httpd_register_uri_handler(server, &ctrlpln);
    httpd_register_uri_handler(server, &ctrltps);
    httpd_register_uri_handler(server, &ctrltpl);
    httpd_register_uri_handler(server, &lplang);
    httpd_register_uri_handler(server, &lplanp);
    httpd_register_uri_handler(server, &stpage);
    httpd_register_uri_handler(server, &stdata);
    httpd_register_uri_handler(server, &sldata);
    httpd_register_uri_handler(server, &stdst);
    httpd_register_uri_handler(server, &otapage);
    httpd_register_uri_handler(server, &otapost);
    httpd_register_uri_handler(server, &otachk);
    httpd_register_uri_handler(server, &otarem);
    httpd_register_uri_handler(server, &rebootu);
    httpd_register_uri_handler(server, &sysrst);
    httpd_register_uri_handler(server, &backupu);
    httpd_register_uri_handler(server, &restore);
    httpd_register_uri_handler(server, &netpage);
    httpd_register_uri_handler(server, &netdata);
    httpd_register_uri_handler(server, &netact);
    httpd_register_uri_handler(server, &bledata);
    httpd_register_uri_handler(server, &bleact);
    httpd_register_uri_handler(server, &blelog);
    httpd_register_uri_handler(server, &wscan);
    httpd_register_uri_handler(server, &wconn);
    httpd_register_uri_handler(server, &wstat);
    httpd_register_uri_handler(server, &ctrlver);

    web_auth_reload();

    sb_prov_cfg_t p;
    sb_prov_load(&p);
    ESP_LOGI(TAG, "Configuration portal available at http://%s/", p.ap_ip);
    return true;
}
