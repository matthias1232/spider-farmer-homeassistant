#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "cJSON.h"

#include "sb_config.h"
#include "mac_filter.h"
#include "dhcp_leases.h"
#include "ip_filter.h"
#include "web_auth.h"
#include "network_page.h"
#include "nav.h"

static const char *TAG = "network_page";

static const char NETWORK_PAGE[] =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>SpiderBridge Network</title><style>"
"*{box-sizing:border-box}"
"body{font-family:system-ui,sans-serif;max-width:46rem;margin:0 auto;"
"padding:1rem;background:#13151a;color:#e6e6e6}"
"h1{font-size:1.3rem;margin:0 0 .2rem}"
"p.sub{margin:0 0 1rem;color:#9aa0aa;font-size:.85rem}"
"a{color:#8ab4f8;text-decoration:none}"
"fieldset{border:1px solid #2c303a;border-radius:.5rem;padding:.9rem;"
"margin:0 0 1rem;background:#1a1d24}"
"legend{color:#8ab4f8;font-size:.9rem;padding:0 .4rem}"
"table{width:100%;border-collapse:collapse;font-size:.82rem}"
"th{text-align:left;color:#9aa0aa;font-weight:500;padding:.3rem .4rem;"
"border-bottom:1px solid #2c303a}"
"td{padding:.35rem .4rem;border-bottom:1px solid #22262e;"
"font-family:ui-monospace,monospace}"
"tr:last-child td{border-bottom:0}"
"input,select{padding:.3rem .45rem;background:#0f1116;color:#e6e6e6;"
"border:1px solid #333845;border-radius:.3rem;font-size:.8rem}"
"button{padding:.3rem .7rem;border:1px solid #333845;border-radius:.3rem;"
"background:#1a1d24;color:#c3c8d0;cursor:pointer;font-size:.8rem}"
"button:hover{background:#242832}"
"button.danger{border-color:#7f1d1d;color:#fca5a5}"
"button.go{background:#8ab4f8;color:#13151a;border-color:#8ab4f8;"
"font-weight:600}"
".row{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap;"
"margin-top:.6rem}"
".on{color:#4ade80}.off{color:#6b7280}.no{color:#f87171}"
".hint{color:#6b7280;font-size:.75rem;margin-top:.5rem;line-height:1.5}"
"#msg{position:fixed;left:50%;bottom:1rem;transform:translateX(-50%);"
"background:#1f2430;border:1px solid #333845;border-radius:.4rem;"
"padding:.5rem .9rem;font-size:.82rem;opacity:0;transition:opacity .2s}"
"#msg.show{opacity:1}";

// Closes the <style> block and opens <body>, and nothing more — the
// nav bar is written immediately after this, before any page content.
static const char NETWORK_BODY_OPEN[] =
"</style></head><body>";

static const char NETWORK_PAGE_BODY[] =
"<h1>Network</h1>"
"<div id=\"app\"><p class=\"sub\">Loading&hellip;</p></div>"
"<div id=\"msg\"></div>"
"<script>"
"let S={};"
"function esc(s){return String(s).replace(/[<>&\"]/g,c=>"
"({'<':'&lt;','>':'&gt;','&':'&amp;','\"':'&quot;'}[c]));}"
"function toast(t){const m=document.getElementById('msg');"
"m.textContent=t;m.className='show';setTimeout(()=>m.className='',2000);}"
"async function act(f){"
"try{const r=await fetch('/network/act',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:new URLSearchParams(f).toString()});"
"const t=await r.text();toast(t);setTimeout(load,400);}"
"catch(e){toast('Connection failed');}}"
"function render(){"
"let h='';"
// --- hotspot clients ---
"h+='<fieldset><legend>Connected to the hotspot</legend>';"
"if(!S.clients.length){h+='<p class=sub>No clients connected.</p>';}"
"else{h+='<table><tr><th>Name</th><th>MAC</th><th>Address</th>"
"<th>Signal</th><th></th></tr>';"
"for(const c of S.clients){"
"h+='<tr><td><input id=\"n_'+c.mac.replace(/:/g,'')+'\" value=\"'+"
"esc(c.name||'')+'\" placeholder=\"unnamed\" size=12></td>'+"
"'<td>'+esc(c.mac)+'</td><td>'+esc(c.ip||'-')+'</td>'+"
"'<td>'+c.rssi+' dBm</td><td>'+"
"'<button onclick=\"nameIt(\\''+c.mac+'\\')\">Save</button> '+"
"(c.listed?'<button class=danger onclick=\"act({a:\\'mac_del\\',"
"mac:\\''+c.mac+'\\'})\">Unlist</button>':"
"'<button onclick=\"act({a:\\'mac_add\\',mac:\\''+c.mac+'\\'})\">"
"List</button>')+'</td></tr>';}"
"h+='</table>';}"
"h+='</fieldset>';"
// --- hotspot access ---
"h+='<fieldset><legend>Who may use the hotspot</legend>';"
"h+='<div class=row><select id=macmode>'+"
"'<option value=0'+(S.mac_mode==0?' selected':'')+'>Open to all</option>'+"
"'<option value=1'+(S.mac_mode==1?' selected':'')+'>Block the listed</option>'+"
"'<option value=2'+(S.mac_mode==2?' selected':'')+'>Allow only the listed</option>'+"
"'</select><button onclick=\"act({a:\\'mac_mode\\',"
"v:document.getElementById(\\'macmode\\').value})\">Apply</button></div>';"
"if(S.mac_list.length){h+='<table><tr><th>MAC</th><th>Note</th><th></th></tr>';"
"for(const m of S.mac_list)"
"h+='<tr><td>'+esc(m.mac)+'</td><td>'+esc(m.label||'')+'</td>'+"
"'<td><button class=danger onclick=\"act({a:\\'mac_del\\',"
"mac:\\''+m.mac+'\\'})\">Remove</button></td></tr>';"
"h+='</table>';}"
"h+='<div class=row><input id=macnew placeholder=\"AA:BB:CC:DD:EE:FF\" "
"size=18><input id=maclbl placeholder=\"note\" size=12>'+"
"'<button onclick=\"act({a:\\'mac_add\\',"
"mac:document.getElementById(\\'macnew\\').value,"
"label:document.getElementById(\\'maclbl\\').value})\">Add</button></div>';"
"h+='<div class=hint>Allow-only mode refuses to turn on with an empty "
"list, since that would lock out the controller too.</div>';"
"h+='</fieldset>';"
// --- web access ---
"h+='<fieldset><legend>Who may open this interface</legend>';"
"h+='<div class=row><select id=ipmode>'+"
"'<option value=0'+(S.ip_mode==0?' selected':'')+'>Open to all</option>'+"
"'<option value=1'+(S.ip_mode==1?' selected':'')+'>Block the listed</option>'+"
"'<option value=2'+(S.ip_mode==2?' selected':'')+'>Allow only the listed</option>'+"
"'</select><button onclick=\"act({a:\\'ip_mode\\',"
"v:document.getElementById(\\'ipmode\\').value})\">Apply</button></div>';"
"if(S.ip_list.length){h+='<table><tr><th>Range</th><th>Note</th><th></th></tr>';"
"for(let i=0;i<S.ip_list.length;i++)"
"h+='<tr><td>'+esc(S.ip_list[i].cidr)+'</td><td>'+"
"esc(S.ip_list[i].note||'')+'</td>'+"
"'<td><button class=danger onclick=\"act({a:\\'ip_del\\',"
"i:'+S.ip_list[i].idx+'})\">Remove</button></td></tr>';"
"h+='</table>';}"
"h+='<div class=row><input id=ipnew placeholder=\"192.168.1.0/24\" size=18>'+"
"'<input id=ipnote placeholder=\"note\" size=12>'+"
"'<button onclick=\"act({a:\\'ip_add\\',"
"cidr:document.getElementById(\\'ipnew\\').value,"
"note:document.getElementById(\\'ipnote\\').value})\">Add</button></div>';"
"h+='<div class=hint>Clients on the SpiderBridge hotspot are always "
"allowed, whatever these rules say — so there is always a way back in "
"if a rule turns out to be wrong.</div>';"
"h+='</fieldset>';"
"document.getElementById('app').innerHTML=h;}"
"function nameIt(mac){"
"const el=document.getElementById('n_'+mac.replace(/:/g,''));"
"act({a:'name',mac:mac,name:el.value});}"
"async function load(){"
"const f=document.activeElement;"
"if(f&&f.tagName==='INPUT')return;"
"try{const r=await fetch('/network/data');S=await r.json();render();}"
"catch(e){}}"
"load();setInterval(()=>{if(!document.hidden)load();},5000);"
"</script></body></html>";

esp_err_t network_page_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, NETWORK_PAGE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NAV_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NETWORK_BODY_OPEN, HTTPD_RESP_USE_STRLEN);
    nav_send(req, NAV_NETWORK);
    httpd_resp_send_chunk(req, NETWORK_PAGE_BODY, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t network_data_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();

    // --- Connected clients ---
    mac_client_t clients[8];
    int n = mac_filter_list_clients(clients, 8);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        char mac_txt[18];
        mac_filter_format(clients[i].mac, mac_txt);

        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "mac", mac_txt);
        cJSON_AddNumberToObject(c, "rssi", clients[i].rssi);
        cJSON_AddBoolToObject(c, "listed", clients[i].listed);

        if (clients[i].ip) {
            const uint8_t *b = (const uint8_t *)&clients[i].ip;
            char ip[16];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            cJSON_AddStringToObject(c, "ip", ip);
        }

        const char *name = dhcp_leases_name_for(clients[i].mac);
        cJSON_AddStringToObject(c, "name", name ? name : "");

        cJSON_AddItemToArray(arr, c);
    }
    cJSON_AddItemToObject(root, "clients", arr);

    // --- Hotspot access list ---
    cJSON_AddNumberToObject(root, "mac_mode", (int)mac_filter_get_mode());
    cJSON *ml = cJSON_CreateArray();
    for (int i = 0; i < MAC_FILTER_MAX_ENTRIES; i++) {
        const mac_filter_entry_t *e = mac_filter_entry_at(i);
        if (!e) continue;
        char mac_txt[18];
        mac_filter_format(e->mac, mac_txt);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mac", mac_txt);
        cJSON_AddStringToObject(o, "label", e->label);
        cJSON_AddItemToArray(ml, o);
    }
    cJSON_AddItemToObject(root, "mac_list", ml);

    // --- Web interface rules ---
    cJSON_AddNumberToObject(root, "ip_mode", (int)ip_filter_get_mode());
    cJSON *il = cJSON_CreateArray();
    for (int i = 0; i < IPF_MAX_RULES; i++) {
        const ip_filter_rule_t *r = ip_filter_at(i);
        if (!r) continue;
        char cidr[20];
        ip_filter_format(r, cidr);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "idx", i);
        cJSON_AddStringToObject(o, "cidr", cidr);
        cJSON_AddStringToObject(o, "note", r->note);
        cJSON_AddItemToArray(il, o);
    }
    cJSON_AddItemToObject(root, "ip_list", il);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"busy\":true}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, strlen(json));
    cJSON_free(json);
    return err;
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) &&
                                isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static bool field(const char *body, const char *key, char *out, size_t out_sz)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *eq = strchr(p, '=');
        if (eq && (!amp || eq < amp) &&
            (size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            size_t len = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
            if (len >= out_sz) len = out_sz - 1;
            memcpy(out, eq + 1, len);
            out[len] = '\0';
            url_decode(out);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

esp_err_t network_act_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Bad request");
        return ESP_OK;
    }

    char body[520];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "Receive failed");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = '\0';

    char act[16] = "", mac[24] = "", label[32] = "", cidr[32] = "", v[8] = "";
    field(body, "a", act, sizeof(act));
    field(body, "mac", mac, sizeof(mac));
    field(body, "label", label, sizeof(label));
    field(body, "cidr", cidr, sizeof(cidr));
    field(body, "v", v, sizeof(v));

    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    if (strcmp(act, "mac_add") == 0) {
        bool ok = mac_filter_add(mac, label);
        return httpd_resp_sendstr(req, ok ? "Added"
                                          : "Could not add that address");
    }
    if (strcmp(act, "mac_del") == 0) {
        bool ok = mac_filter_remove(mac);
        return httpd_resp_sendstr(req, ok ? "Removed" : "Not found");
    }
    if (strcmp(act, "mac_mode") == 0) {
        int m = atoi(v);
        if (m < FILTER_OFF || m > FILTER_WHITELIST) {
            return httpd_resp_sendstr(req, "Unknown mode");
        }
        bool ok = mac_filter_set_mode((mac_filter_mode_t)m);
        return httpd_resp_sendstr(req, ok ? "Mode applied"
            : "Refused: allow-only with an empty list would block everything");
    }
    if (strcmp(act, "name") == 0) {
        char name[48] = "";
        field(body, "name", name, sizeof(name));
        bool ok = dhcp_leases_set(mac, name, NULL);
        return httpd_resp_sendstr(req, ok ? "Name saved" : "Could not save");
    }
    if (strcmp(act, "ip_add") == 0) {
        char note[32] = "";
        field(body, "note", note, sizeof(note));
        bool ok = ip_filter_add(cidr, note);
        return httpd_resp_sendstr(req, ok ? "Rule added"
                                          : "Could not read that range");
    }
    if (strcmp(act, "ip_del") == 0) {
        char idx[8] = "";
        field(body, "i", idx, sizeof(idx));
        bool ok = ip_filter_remove(atoi(idx));
        return httpd_resp_sendstr(req, ok ? "Rule removed" : "Not found");
    }
    if (strcmp(act, "ip_mode") == 0) {
        int m = atoi(v);
        if (m < IPF_OFF || m > IPF_ALLOWLIST) {
            return httpd_resp_sendstr(req, "Unknown mode");
        }
        bool ok = ip_filter_set_mode((ip_filter_mode_t)m);
        return httpd_resp_sendstr(req, ok ? "Mode applied"
            : "Refused: allow-only with no rules would block everything");
    }

    ESP_LOGW(TAG, "Unknown network action '%s'", act);
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "Unknown action");
}
