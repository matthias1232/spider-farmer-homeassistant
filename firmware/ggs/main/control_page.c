#include <string.h>
#include <strings.h>
#include <time.h>
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "sb_config.h"
#include "provisioning.h"
#include "device_cache.h"
#include "device_registry.h"
#include "sf_command_handler.h"
#include "sf_normalizer.h"
#include "sf_alarm.h"
#include "mitm_proxy.h"
#include "ha_mqtt.h"
#include "web_auth.h"
#include "tz_table.h"
#include "control_page.h"
#include "plan_templates.h"
#include "plan_store.h"
#include "plan_text.h"
#include "sf_normalizer.h"
#include "nav.h"

static const char *TAG = "control";

// ---------------------------------------------------------------------------
// The page itself. Controls are built from the JSON feed so the layout
// stays in one place and the firmware does not render HTML per field.
// ---------------------------------------------------------------------------
static const char CONTROL_PAGE[] =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>SpiderBridge Control</title><style>"
"*{box-sizing:border-box}"
"body{font-family:system-ui,sans-serif;max-width:40rem;margin:0 auto;"
"padding:1rem;background:#13151a;color:#e6e6e6}"
"h1{font-size:1.3rem;margin:0 0 .2rem}"
"p.sub{margin:0 0 1rem;color:#9aa0aa;font-size:.85rem}"
"a{color:#8ab4f8;text-decoration:none}"
"fieldset{border:1px solid #2c303a;border-radius:.5rem;padding:.9rem;"
"margin:0 0 1rem;background:#1a1d24}"
"legend{color:#8ab4f8;font-size:.9rem;padding:0 .4rem}"
".row{display:flex;align-items:center;gap:.6rem;padding:.4rem 0;"
"border-bottom:1px solid #22262e;flex-wrap:wrap}"
".row:last-child{border-bottom:0}"
".lbl{flex:1;min-width:8rem;font-size:.88rem}"
".val{color:#9aa0aa;font-size:.82rem;min-width:3.5rem;text-align:right}"
// What is in force right now, beside every control.
".now{display:inline-block;min-width:3rem;padding:.1rem .4rem;margin-right:.35rem;border-radius:.3rem;"
"background:#0f2a1a;color:#4ade80;font-size:.8rem;text-align:center;border:1px solid #14532d}"
"button{padding:.35rem .8rem;border:1px solid #333845;border-radius:.3rem;"
"background:#1a1d24;color:#c3c8d0;cursor:pointer;font-size:.82rem}"
"button.on{background:#4ade80;color:#13151a;border-color:#4ade80;font-weight:600}"
"button.off{background:#2c303a;color:#9aa0aa}"
"input[type=range]{flex:1;min-width:7rem;accent-color:#8ab4f8}"
"input[type=text],input[type=number],select{padding:.3rem .5rem;"
"background:#0f1116;color:#e6e6e6;border:1px solid #333845;"
"border-radius:.3rem;font-size:.82rem;width:6.5rem}"
"select{width:auto;max-width:14rem}"
// The three parts of a time, grouped so the colons sit between them rather
// than between full-width boxes. The parts are narrower than the usual
// select because a full-width hour list pushed the seconds off the row.
".tbox{display:inline-flex;align-items:center;gap:.15rem}"
".tbox select{width:3.4rem;padding:.3rem .2rem;text-align:center}"
"#msg{position:fixed;left:50%;bottom:1rem;transform:translateX(-50%);"
"background:#1a1d24;border:1px solid #2c303a;border-radius:.4rem;"
"padding:.5rem 1rem;font-size:.85rem;opacity:0;transition:opacity .3s;"
"pointer-events:none}"
"#msg.show{opacity:1}"
".none{color:#6b7280;font-size:.85rem;padding:.5rem 0}"
// Toggle switch, matching the settings and status pages so the same
// kind of setting looks the same wherever it appears.
".sw{position:relative;display:inline-block;width:2.4rem;height:1.3rem;"
"flex:none}"
".sw input{opacity:0;width:0;height:0}"
".sw span{position:absolute;inset:0;background:#333845;border-radius:1rem;"
"cursor:pointer;transition:.15s}"
".sw span:before{content:'';position:absolute;width:1rem;height:1rem;"
"left:.15rem;top:.15rem;background:#9aa0aa;border-radius:50%;"
"transition:.15s}"
".sw input:checked+span{background:#8ab4f8}"
".sw input:checked+span:before{transform:translateX(1.1rem);background:#fff}"
".sw input:disabled+span{opacity:.4;cursor:not-allowed}"
".tabs{display:flex;gap:.3rem;flex-wrap:wrap;margin:0 0 1rem;"
"border-bottom:1px solid #2c303a;padding-bottom:.5rem}"
".tab{padding:.4rem .8rem;border:1px solid #333845;border-radius:.3rem .3rem 0 0;"
"background:#1a1d24;color:#9aa0aa;cursor:pointer;font-size:.85rem;"
"display:flex;align-items:center;gap:.4rem}"
".tab.sel{background:#8ab4f8;color:#13151a;border-color:#8ab4f8;font-weight:600}"
".dot{width:.5rem;height:.5rem;border-radius:50%;background:#6b7280;"
"flex-shrink:0}"
".dot.up{background:#4ade80}"
".devbar{display:flex;gap:.5rem;align-items:center;margin:0 0 1rem;"
"flex-wrap:wrap;font-size:.82rem;color:#9aa0aa}"
".devbar input{width:9rem}"
".devinfo{font-size:.75rem;color:#9ca3af;margin:-.6rem 0 1rem}"
"button.danger{border-color:#7f1d1d;color:#fca5a5}"
"button.danger:hover{background:#7f1d1d;color:#fff}";

// Closes the <style> block and opens <body>, and nothing more -- the
// nav bar is written immediately after this, before any page content.
static const char CONTROL_BODY_OPEN[] =
"</style></head><body>";

static const char CONTROL_PAGE_BODY[] =
"<h1>Control</h1>"
"<p class=\"sub\">Everything the controller exposes.</p>"
"<div id=\"tabs\" class=\"tabs\"></div>"
"<div id=\"app\"><div class=\"none\">Loading&hellip;</div></div>"
"<div id=\"msg\"></div>"
"<script>"
"let S={};"
// MAC of the tab being shown. It is the identity that survives renaming,
// so the selection sticks even while the user is typing a new name.
"let SEL='';"
// Longer messages stay longer, so an explanation can be read.
"function toast(t){const m=document.getElementById('msg');"
"m.textContent=t;m.className='show';clearTimeout(toast.t);toast.t=setTimeout(()=>m.className='',t.length>20?6000:1800);}"
"function dev(){return (S.devices||[]).find(d=>d.mac===SEL);}"
"async function send(f,s,v){"
"try{const r=await fetch('/control/set',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'d='+encodeURIComponent(SEL)+"
"'&f='+encodeURIComponent(f)+'&s='+encodeURIComponent(s||'')+"
"'&v='+encodeURIComponent(v)});"
"const t=await r.text();toast(r.ok?(t==='ok'?'Sent':t):'Not sent: '+t);"
// A refused change goes back to what is in force right away.
"if(!r.ok){for(const e of document.querySelectorAll('[data-hold]'))delete e.dataset.hold;render();}"
// The bridge marks the change at once and the controller confirms it a
// second or two later: ask for news a few times right after sending.
"setTimeout(checkVer,300);setTimeout(checkVer,1500);setTimeout(checkVer,4000);}catch(e){toast('Connection lost');}}"
// Renaming changes the MQTT topics and the Home Assistant entity names.
// The controller keeps its MAC, so nothing the vendor cloud sees changes.
"async function rename(){"
"const d=dev();if(!d)return;"
"const name=document.getElementById('dname').value.trim();"
"const slug=document.getElementById('dslug').value.trim();"
"try{const r=await fetch('/control/device',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'a=rename&d='+encodeURIComponent(d.mac)+"
"'&name='+encodeURIComponent(name)+'&slug='+encodeURIComponent(slug)});"
"const t=await r.text();toast(r.ok?'Renamed':'Failed: '+t);"
"setTimeout(load,600);}catch(e){toast('Connection lost');}}"

// Pairing off = setDevDeactive over the controller's own session. The
// controller keeps its Wi-Fi and advertises over Bluetooth again.
"async function unpair(){"
"const d=dev();if(!d)return;"
"if(!confirm('Remove the Bluetooth pairing of '+d.name+'?\\n\\nThe controller advertises over "
"Bluetooth again, so a phone (Spider Farmer app) can pair with it. Its Wi-Fi settings and its "
"connection to this bridge stay as they are.'))return;"
"send('pairing','','OFF');}"
"async function forget(){"
"const d=dev();if(!d)return;"
"if(!confirm('Remove '+d.name+'? It comes back automatically if that "
"controller reports again.'))return;"
"try{const r=await fetch('/control/device',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'a=forget&d='+encodeURIComponent(d.mac)});"
"await r.text();SEL='';toast('Removed');"
"setTimeout(load,400);}catch(e){toast('Connection lost');}}"
"function esc(s){return String(s).replace(/[<>&\"]/g,c=>"
"({'<':'&lt;','>':'&gt;','&':'&amp;','\"':'&quot;'}[c]));}"
// --- In-place updates instead of rebuilding the page ---
//
// render() used to replace the whole markup on every refresh. That closed an
// open drop-down, dropped a half-made choice and made the page jump. Now the
// new markup is built off-screen and merged into the live page: only text,
// attributes and values that differ are touched, and elements are matched by
// id, by fieldset legend or by row label, so a row appearing elsewhere does
// not shift the others.
//
// Left alone during a merge:
//   data-busy   a drop-down or slider the user is operating right now
//   data-hold   a control just changed and sent: shows the new value until
//               the controller has confirmed it (a few seconds)
//   data-dirty  a choice not sent yet (fields with a Set/Save button):
//               kept until the bridge reports that same value
//   data-frozen an open editor (same data-k), its fields are user state
//   data-own    an element whose options are filled by script (zone list)
//   focused text inputs while typing
"function nkey(e){if(e.nodeType!==1)return null;if(e.dataset.k)return 'k:'+e.dataset.k;if(e.id)return 'i:'+e.id;"
"if(e.tagName==='FIELDSET'){const l=e.querySelector(':scope>legend');return l?'f:'+l.textContent:null;}"
"if(e.classList.contains('row')||e.classList.contains('r')){const l=e.querySelector(':scope>.lbl');return l?'r:'+l.textContent:null;}"
"return null;}"
// busy counts only while the control still has the focus: a drop-down
// opened and closed without a choice must not stay frozen.
"function held(e){if(e.nodeType!==1)return false;"
"if(e.dataset.busy){if(e===document.activeElement)return true;delete e.dataset.busy;}"
"if(e.dataset.hold){if(+e.dataset.hold>Date.now())return true;delete e.dataset.hold;}return false;}"
"const KEEPA=['data-hold','data-busy','data-dirty','data-t0'];"
"function syncAttrs(o,n){for(const a of Array.from(o.attributes)){if(KEEPA.includes(a.name))continue;"
"if(!n.hasAttribute(a.name))o.removeAttribute(a.name);}"
"for(const a of Array.from(n.attributes)){if(o.getAttribute(a.name)!==a.value)o.setAttribute(a.name,a.value);}}"
"function syncVal(o,n){const t=o.tagName;if(t!=='SELECT'&&t!=='INPUT'&&t!=='TEXTAREA')return;"
"const cb=o.type==='checkbox'||o.type==='radio';const nv=cb?n.checked:n.value,ov=cb?o.checked:o.value;"
"if(o.dataset.dirty){if(nv===ov)delete o.dataset.dirty;return;}"
"if(o===document.activeElement&&t!=='SELECT'&&!cb)return;"
"if(nv!==ov){if(cb)o.checked=nv;else o.value=nv;}}"
"function patch(o,n){"
"if(o.nodeType!==1){if(o.nodeValue!==n.nodeValue)o.nodeValue=n.nodeValue;return;}"
"if(held(o)||o.dataset.own)return;"
"if(o.dataset.frozen&&o.dataset.k===n.dataset.k)return;"
"if(o.dataset.s!==n.dataset.s)delete o.dataset.t0;"
"syncAttrs(o,n);"
// Dirty selects too: morph() refreshes their option list (a new template
// appears) and keeps the user's unsent choice when it still exists.
"morph(o,n);syncVal(o,n);}"
"function morph(o,n){"
// A select's option list is replaced as a whole when it differs (Auto
// appearing or disappearing); options carry no keys of their own, and a
// partial merge could leave a stale entry selected.
"if(o.tagName==='SELECT'){if(o.innerHTML!==n.innerHTML){const v=o.value;o.innerHTML=n.innerHTML;"
"if(o.dataset.dirty&&[...o.options].some(x=>x.value===v))o.value=v;}return;}"
"const map=new Map();"
"for(const c of Array.from(o.childNodes)){const k=nkey(c);if(k){if(!map.has(k))map.set(k,[]);map.get(k).push(c);}}"
"const used=new Set();let i=0;"
"for(const c of Array.from(n.childNodes)){"
"const k=nkey(c);let m=null;"
"if(k){const q=map.get(k);while(q&&q.length){const x=q.shift();if(!used.has(x)){m=x;break;}}}"
"else{const cur=o.childNodes[i];if(cur&&!used.has(cur)&&!nkey(cur)&&cur.nodeName===c.nodeName)m=cur;}"
"if(m&&m.nodeName!==c.nodeName)m=null;"
"if(m){used.add(m);if(o.childNodes[i]!==m)o.insertBefore(m,o.childNodes[i]||null);patch(m,c);}"
"else{o.insertBefore(c,o.childNodes[i]||null);used.add(c);}"
"i++;}"
"while(o.childNodes.length>i)o.removeChild(o.lastChild);}"
"function morphHTML(el,h){const t=document.createElement(el.tagName);t.innerHTML=h;morph(el,t);}"
// Marks a control while it is being operated, and what happens after.
"document.addEventListener('pointerdown',e=>{const t=e.target.closest&&e.target.closest('select,input[type=range]');"
"if(t)t.dataset.busy=1;},true);"
"document.addEventListener('focusout',e=>{const t=e.target;if(t&&t.dataset)delete t.dataset.busy;},true);"
"document.addEventListener('keyup',e=>{const a=document.activeElement;if(e.key==='Escape'&&a&&a.dataset)delete a.dataset.busy;});"
// A control that sends on change shows the chosen value right away and
// keeps it until the controller confirms; one with a separate Set/Save
// button keeps the user's choice until it is sent.
"document.addEventListener('change',e=>{const t=e.target;if(!t||!t.dataset)return;delete t.dataset.busy;"
"if(t.hasAttribute('onchange')){t.dataset.hold=Date.now()+6000;setTimeout(render,6200);}else t.dataset.dirty=1;},true);"
"document.addEventListener('input',e=>{const t=e.target;if(t&&t.tagName==='INPUT'&&t.type==='text')t.dataset.dirty=1;},true);"
// ON/OFF buttons flip at once (the command is already on its way). The
// button's own command is turned round too, so a second click sends the
// opposite again.
"document.addEventListener('click',e=>{const b=e.target.closest&&e.target.closest('button.on,button.off');"
"if(!b||!/^(ON|OFF)$/.test(b.textContent))return;"
"const on=b.classList.contains('on');b.classList.toggle('on',!on);b.classList.toggle('off',on);b.textContent=on?'OFF':'ON';"
"const oc=b.getAttribute('onclick');"
"if(oc)b.setAttribute('onclick',oc.replace(/(ON|OFF)'\\)/,m=>m.indexOf('ON')===0?'OFF'+m.slice(2):'ON'+m.slice(3)));"
"b.dataset.hold=Date.now()+6000;setTimeout(render,6200);});"
"function toggle(f,cur){return '<button class=\"'+(cur?'on':'off')+'\" '+"
"'onclick=\"send(\\''+f+'\\',\\'\\',\\''+(cur?'OFF':'ON')+'\\')\">'+"
"(cur?'ON':'OFF')+'</button>';}"
"function slider(f,sub,v,min,max){"
// The badge shows what is in force; the number after the slider follows
// the slider while it is dragged.
"return now(v)+'<input type=range min='+min+' max='+max+' value='+v+' '+"
"'oninput=\"this.nextElementSibling.textContent=this.value\" '+"
"'onchange=\"send(\\''+f+'\\',\\''+sub+'\\',this.value)\">'+"
"'<span class=val>'+v+'</span>';}"
// One clock for every time field on the page.
//
// It replaces the free-text boxes, which asked for the format in a
// placeholder and rejected nothing: a mistyped time was sent as typed and
// the controller either refused it or stored something else.
//
// Layout: the time the controller currently has, then hour : minute
// [: second] selects that are always visible. Changing any of them sends
// the whole time at once, like every other select on the page -- no Set
// button. withSec adds the seconds column, for cycle run and cycle off
// only: the app sets those to the second.
//
// The selects used to hide behind a click and wait for a Set button. The
// page re-renders every 5 seconds, which closed the picker mid-selection,
// so a time could not be set at all.
//
// A native time input was not used on purpose: it has no seconds step, and
// its value format and keyboard behaviour differ between platforms. These
// selects behave identically everywhere.
"function clock(f,sub,val,withSec){"
"const p=String(val||'00:00').split(':');"
"const H=p[0]||'00',M=p[1]||'00',S=p[2]||'00';"
"const id='c'+f+'_'+sub;"
"const pad=n=>String(n).padStart(2,'0');"
"const hh=Array.from({length:24},(_,i)=>pad(i));"
"const mm=Array.from({length:60},(_,i)=>pad(i));"
"const qs=(c,o)=>'<select onchange=\"sendClock(\\''+id+'\\')\">'+"
"o.map(v=>'<option'+(v===c?' selected':'')+'>'+v+'</option>').join('')+'</select>';"
"let x='<span class=clock id='+id+' data-f='+f+' data-s='+sub+' data-sec='+(withSec?1:0)+'>';"
"x+=now(val||'00:00')+' ';"
"x+=qs(H,hh)+':'+qs(M,mm);"
"if(withSec)x+=':'+qs(S,mm);"
"return x+'</span>';}"
// Sends the clock's full value as soon as one of its selects changes.
"async function sendClock(id){"
"const wrap=document.getElementById(id);"
"if(!wrap)return;"
"const b=wrap.querySelectorAll('select');"
"if(b.length<2)return;"
"let v=b[0].value+':'+b[1].value;"
"if(wrap.dataset.sec==='1')v+=':'+(b[2]?b[2].value:'00');"
// The selects keep the new choice while it is on its way; the value on the
// left stays what is in force until the controller confirms.
"for(const s of b)s.dataset.hold=Date.now()+6000;"
"await send(wrap.dataset.f,wrap.dataset.s,v);}"
// What is in force right now, as the bridge (and so Home Assistant) has
// it, shown next to every control. While a change is on its way the
// control already shows the new choice and this still the old value, until
// the controller confirms it.
"function now(v){return (v===undefined||v===null||v==='')?'':'<span class=now title=\"Active now (controller / MQTT)\">'+esc(v)+'</span>';}"
"function sel(f,sub,cur,opts){"
"let h=now(cur)+'<select onchange=\"send(\\''+f+'\\',\\''+sub+'\\',this.value)\">';"
// String compare: options are strings, cached values often numbers (fan
// level 3 never matched '3', so the list always showed its first entry).
"for(const o of opts)h+='<option'+(String(o)===String(cur)?' selected':'')+'>'+esc(o)+'</option>';"
"return h+'</select>';}"
// A select over a range of numbers, so a value that cannot go wrong is
// chosen instead of typed.
//
// The current value is kept even when it is outside the list rather than
// snapped to the nearest option: a select showing "5" as selected while
// the controller holds 12 would write 5 the moment anything else on the
// page was touched.
//
// A value the controller has never reported is left out entirely. It
// would otherwise be added as an option reading "undefined", and picking
// it would send the string "undefined" to the firmware.
// Counted in whole steps and formatted with a fixed number of decimals:
// adding 0.1 repeatedly in floating point drifts (-9.799999999999999), so
// the list showed long fractions and the current value never matched an
// option.
"function selRange(f,sub,cur,lo,hi,step){"
"step=step||1;"
"const dp=step<1?String(step).split('.')[1].length:0;"
"const fmt=v=>(Math.round(v*Math.pow(10,dp))/Math.pow(10,dp)).toFixed(dp);"
"let opts=[];"
"const n=Math.round((hi-lo)/step);"
"for(let i=0;i<=n;i++)opts.push(fmt(lo+i*step));"
"if(cur===undefined||cur===null)return sel(f,sub,'',opts);"
"let c=typeof cur==='number'?fmt(cur):String(cur);"
"if(opts.indexOf(c)<0){opts.push(c);opts.sort((a,b)=>parseFloat(a)-parseFloat(b));}"
"return sel(f,sub,c,opts);}"
// A select that changes nothing on its own, for fields that the
// controller only accepts as part of a complete block. It is written into
// the markup and read back when the group is submitted.
// A range with named values ahead of it, for fields where Off is a real
// setting rather than the number zero: the light's dim and off
// thresholds, and the sunrise/sunset ramp.
//
// The firmware takes "Off" and writes the controller's own 0 for it, so
// the word round-trips rather than needing a translation table here.
"function selOpts(f,sub,cur,named,lo,hi){"
"let opts=named.slice();"
"for(let v=lo;v<=hi;v++)opts.push(String(v));"
"if(cur===undefined||cur===null)return sel(f,sub,'',opts);"
"let c=(cur===0)?named[0]:String(cur);"
"if(opts.indexOf(c)<0){opts.push(c);opts.sort((a,b)=>parseFloat(a)-parseFloat(b));}"
"return sel(f,sub,c,opts);}"
// No empty "--" entry: the controller has no "unset" for these, so a value
// not reported yet shows the lowest allowed one (0 for targets) instead of
// an option that could be submitted as nothing. A reported value outside
// the list is clamped into it rather than added.
"function pickNum(cur,lo,hi,step,id){"
"step=step||1;"
"let opts=[];"
"for(let v=lo;v<=hi;v+=step)opts.push(String(v));"
"let n=parseFloat(cur);"
"if(isNaN(n))n=lo;"
"n=Math.min(hi,Math.max(lo,Math.round((n-lo)/step)*step+lo));"
"const c=String(n);"
"let h=now(cur===undefined?'':c)+'<select id='+id+'>';"
"for(const o of opts)h+='<option'+(o===c?' selected':'')+'>'+esc(o)+'</option>';"
"return h+'</select>';}"
// An ON/OFF switch at the start of a row also gets the in-force badge: the
// button itself flips at once when clicked, the badge follows only when
// the controller has confirmed.
"function row(l,ctrl){const m=/^<button class=\"(?:on|off)\"[^>]*>(ON|OFF|--)</.exec(ctrl);"
"return '<div class=row><span class=lbl>'+l+'</span>'+(m?now(m[1]):'')+ctrl+'</div>';}"
"const FANMODES=['Manual','Schedule','Cycle',"
"'Environment: Prioritize temperature','Environment: Prioritize humidity',"
"'Environment: Temperature only','Environment: Humidity only',"
"'Environment: Temperature & humidity'];"
"const LIGHTMODES=['Manual','Schedule','PPFD'];"
// fanBlock takes both ends of the module's range rather than just the
// top: the circulation fan runs 1-10, the exhaust blower 25-100. Only the
// upper bound alone would offer blower speeds the controller rejects, and
// a rejected write leaves the old value in place without any message.
// Speed is a list, not a slider, for the same reason the app uses one:
// the blower's running speed includes Auto, which is not a speed at all
// and so has nowhere to sit on a slider. Percents in steps of ten, as the
// app shows them; the firmware scales the fan's onto its own 1-10.
// Percent ranges matching what the firmware accepts, so every value the
// page offers is one the controller will take.
// In each module's own units, as the app shows them: the circulation fan
// 1-10, the exhaust blower 25-100 in steps of 1.
"FANPCT={fan:[1,10],blower:[25,100]};"
"function fanBlock(D,key,title){"
"const b=D[key];if(!b)return '';"
"const p=FANPCT[key];"
"let opts=[];"
"for(let v=p[0];v<=p[1];v++)opts.push(String(v));"
// Auto exists only for the blower's schedule speed; the running speed is
// always the number the controller holds.
// Schedule speed: Auto plus the module's own range (fan 1-10, exhaust
// 25-100 % in 1 % steps).
"const sopts=['Auto'].concat(opts);"
"const envm=String(b.mode_label||'').indexOf('Environment')===0;"
"const manual=!b.mode_label||b.mode_label==='Manual';"
"let h='<fieldset><legend>'+title+'</legend>';"
"h+=row('Power',toggle(key,b.on));"
// The running speed is only settable in Manual mode; otherwise the
// controller chooses it and the page just shows it.
"h+=row('Speed',manual?sel(key,'percentage',b.level,opts)"
":now(b.level+(key==='blower'?' %':''))+'<span class=none>read-only outside Manual mode</span>');"
"h+=row('Mode',sel(key,'preset_mode',b.mode_label,FANMODES));"
// Oscillation and natural wind are circulation-fan fields; CO2 is an
// exhaust-blower field. The captured traffic never mixes them, so
// offering the wrong one would write a key the controller does not know
// for that module.
// Oscillation as the app shows it: a switch, and a level 1-10 (wire
// shakeLevel 0 = off). The level is shown even while off (last one used).
"if(key==='fan')h+=row('Oscillation','<button class=\"'+(b.shake>0?'on':'off')+'\" '+"
"'onclick=\"send(\\''+key+'\\',\\'oscillation\\',\\''+(b.shake>0?'OFF':'ON')+'\\')\">'+"
"(b.shake>0?'ON':'OFF')+'</button>');"
"if(key==='fan')h+=row('Oscillation level',selRange(key,'oscillation_level',b.shake>0?b.shake:(b.shake_last||5),1,10,1));"
"if(key==='fan')h+=row('Natural wind','<button class=\"'+(b.natural?'on':'off')+'\" '+"
"'onclick=\"send(\\''+key+'\\',\\'natural_wind\\',\\''+(b.natural?'OFF':'ON')+'\\')\">'+"
"(b.natural?'ON':'OFF')+'</button>');"
// Shown even before the controller has reported closeCO2, so the control
// is not simply missing on a fresh boot. Unknown reads as "--" and the
// button still sends, which is how the value first gets established.
//
// The label names what ON does -- hold the CO2 inlet back while the
// blower runs -- because "CO2 connection" next to an ON button read as
// though ON opened the valve. The field has always meant the opposite.
"if(key==='blower')h+=row('Close CO2 while blower runs',"
"'<button class=\"'+(b.co2===undefined?'off':(b.co2?'on':'off'))+'\" '+"
"'onclick=\"send(\\''+key+'\\',\\'close_co2\\',\\''+(b.co2?'OFF':'ON')+'\\')\">'+"
"(b.co2===undefined?'--':(b.co2?'ON':'OFF'))+'</button>');"
// Schedule speed runs over the same list as the running speed, Auto
// included, because it is the same choice for the scheduled modes.
//
// Standby is a narrower thing in the app: Off or a low fixed speed. The
// blower's band is 25-39. The circulation fan's standby is fixed at 0 with
// no control at all -- its own range starts at 1, so there is no lower
// value to hold and every reachable value would be a running speed.
// Auto only in Environment mode. Switching to another mode with Auto set
// makes the bridge change it to the lowest step (and say so); should the
// controller still report Auto outside Environment, it is flagged here.
"h+=row('Schedule speed',sel(key,'schedule_speed',b.maxSpeed,"
"envm||b.maxSpeed==='Auto'?sopts:sopts.slice(1))"
"+(envm?'':(b.maxSpeed==='Auto'?'<span class=none style=\"color:#fbbf24\">Auto is not valid outside Environment mode -- pick a speed</span>'"
":'<span class=none>Auto only in Environment mode</span>')));"
"h+=row('Standby speed',key==='fan'"
"?now('Off')+'<span class=none>always 0</span>'"
":sel(key,'standby_speed',b.minSpeed,['Off'].concat("
"Array.from({length:15},(_,i)=>String(25+i)))));"
"h+=row('Schedule start',clock(key,'schedule_start',b.sched_start,0));"
"h+=row('Schedule end',clock(key,'schedule_end',b.sched_end,0));"
"h+=row('Cycle start',clock(key,'cycle_start',b.cyc_start,0));"
// Seconds matter here: the app sets these to the second, so the minute
// fields alone would quietly round a 03h02min02s run time down.
"h+=row('Cycle run',clock(key,'cycle_run_time',b.cyc_run_t,1));"
"h+=row('Cycle off',clock(key,'cycle_off_time',b.cyc_off_t,1));"
// Cycle times run to the controller's own cap of 100. It used to be a
// 1-3 list here, which left every value between 4 and 100 unreachable.
"h+=row('Cycle repeats',selRange(key,'cycle_times',b.cyc_times,1,100));"
"return h+'</fieldset>';}"
"function lightBlock(D,key,title){"
"const b=D[key];if(!b)return '';"
"let h='<fieldset><legend>'+title+'</legend>';"
"h+=row('Power',toggle(key,b.on));"
"h+=row('Brightness',slider(key,'brightness',b.level,0,100));"
"h+=row('Mode',sel(key,'effect',b.mode_label,LIGHTMODES));"
// Brightness floors at 11%: that is the lowest target the app offers, and
// a control below it would only offer values the app never writes.
"h+=row('Schedule brightness',selRange(key,'schedule_brightness',b.sched_bri,11,100));"
"h+=row('Schedule start',clock(key,'schedule_start',b.sched_start,0));"
"h+=row('Schedule end',clock(key,'schedule_end',b.sched_end,0));"
// Sunrise and sunset simulation: Off, or 1-60 minutes. Off is the
// controller's own 0, which it reads as "no ramp".
"h+=row('Sunrise/sunset (min)',selOpts(key,'fade_minutes',b.fade,['Off'],1,60));"
// Dim and off thresholds: Off, or 15-50 degrees C.
"h+=row('Dim threshold',selOpts(key,'dim_threshold',b.dark,['Off'],15,50));"
"h+=row('Off threshold',selOpts(key,'off_threshold',b.offT,['Off'],15,50));"
// PPFD target in micromoles, to 2000. The earlier cap of 1000 was where
// the sensor's reporting scale stops, not something the controller
// enforces -- a target above that is a light meant to outrun the sensor,
// which is the point of setting one.
"h+=row('PPFD target',selRange(key,'ppfd_target',b.ppfd,20,2000,10));"
"h+=row('PPFD start',clock(key,'ppfd_start',b.ppfd_start,0));"
"h+=row('PPFD end',clock(key,'ppfd_end',b.ppfd_end,0));"
"h+=row('PPFD fade (min)',selOpts(key,'ppfd_fade_minutes',b.ppfd_fade,['Off'],1,60));"
"h+=row('PPFD min %',selRange(key,'ppfd_min',b.ppfd_min,11,100));"
"h+=row('PPFD max %',selRange(key,'ppfd_max',b.ppfd_max,11,100));"
"return h+'</fieldset>';}"
"function renderTabs(){"
"const t=document.getElementById('tabs');"
"const ds=S.devices||[];"
"if(!ds.length){morphHTML(t,'');return;}"
"let h='';"
"for(const d of ds){"
"h+='<div class=\"tab'+(d.mac===SEL?' sel':'')+'\" '+"
"'onclick=\"pick(\\''+d.mac+'\\')\">'+"
"'<span class=\"dot'+(d.online?' up':'')+'\"></span>'+esc(d.name)+'</div>';}"
"morphHTML(t,h);}"
"function pick(mac){SEL=mac;render();}"
"function render(){"
"const a=document.getElementById('app');"
"const ds=S.devices||[];"
"if(!ds.length){"
"morphHTML(document.getElementById('tabs'),'');"
"morphHTML(a,'<div class=none>No controller known yet. One appears here "
"as soon as it connects through the bridge.</div>');return;}"
// Keep the chosen tab across refreshes; fall back to the first device
// when the selected one was just removed.
"if(!ds.some(d=>d.mac===SEL))SEL=ds[0].mac;"
"renderTabs();"
"const D=dev();if(!D)return;"
"let h='';"
// Rename and remove, shown per tab.
// Fields hold only what the user set; a field left at the default shows
// empty with the default as placeholder. Typed values survive the
// periodic refresh while focused. The line below shows, read-only, what is
// actually in use -- including the MAC-based default for empty fields.
"{const keep=(id,v)=>{const e=document.getElementById(id);"
"return (e&&document.activeElement===e)?e.value:v;};"
"const nv=keep('dname',D.name===D.def_name?'':D.name);"
"const sv=keep('dslug',D.slug===D.def_slug?'':D.slug);"
"h+='<div class=devbar>'+"
"'<span>Name</span><input id=dname type=text placeholder=\"'+esc(D.def_name)+'\" value=\"'+esc(nv)+'\">'+"
"'<span>Topic</span><input id=dslug type=text placeholder=\"'+esc(D.def_slug)+'\" value=\"'+esc(sv)+'\">'+"
"'<button onclick=\"rename()\">Save</button>'+"
"'<button class=danger onclick=\"forget()\">Remove</button>'+"
"'<button onclick=\"unpair()\"'+(D.online?'':' disabled')+' title=\"Removes the Bluetooth pairing: the "
"controller advertises again so a phone can pair with it. Wi-Fi settings stay.\">Unpair (Bluetooth)</button>'+"
"'<span>'+esc(D.mac)+'</span></div>'+"
"'<div class=devinfo>In use: name <b>'+esc(D.name)+'</b>'+(D.name===D.def_name?' (default)':'')+"
"', topic <code>spiderfarmer/'+esc(D.slug)+'/&hellip;</code>'+(D.slug===D.def_slug?' (default)':'')+"
"'. Leave a field empty and save to go back to the default with the MAC.</div>';}"
"if(!D.online)h+='<div class=none>This controller is not connected right "
"now. The values below are the last ones it reported.</div>';"
"if(D.sensors&&Object.keys(D.sensors).length){"
"h+='<fieldset><legend>Sensors</legend>';"
"for(const[k,v]of Object.entries(D.sensors))"
"h+=row(k,'<span class=val>'+esc(v)+'</span>');"
"h+='</fieldset>';}"
"h+=fanBlock(D,'fan','Fan Circulation');"
"h+=fanBlock(D,'blower','Fan Exhaust');"
"h+=lightBlock(D,'light','Light');"
"h+=lightBlock(D,'light2','Light 2');"
"if(D.outlets&&D.outlets.length){"
"h+='<fieldset><legend>Outlets</legend>';"
"for(const o of D.outlets)"
"h+=row('Outlet '+o.n,toggle('outlet_'+o.n,o.on));"
"h+='</fieldset>';}"
"if(D.climate&&D.climate.length){"
"h+='<fieldset><legend>Climate</legend>';"
"for(const c of D.climate)"
"h+=row(c.name,toggle(c.key,c.on));"
"h+='</fieldset>';}"
// --- Sensor cleaning ---
//
// The two-hour cycle and the five-minute cooldown are the controller's own,
// so this is a switch plus a countdown rather than a start button with a
// duration field: there is nothing here to schedule.
"if(D.clean){"
"const c=D.clean;"
"h+='<fieldset><legend>Sensor cleaning</legend>';"
// One C string: the JS concatenation is escaped inside it, since the C
// literal cannot be broken by bare + expressions.
"h+=row('Clean temperature/humidity sensor','<button class=\"'+(c.on?'on':'off')+"
"'\" onclick=\"send(\\'sensor_cleaning\\',\\'\\',\\''+(c.on?'OFF':'ON')+"
"'\\')\">'+(c.on?'ON':'OFF')+'</button>');"
"const hms=s=>Math.floor(s/3600)+':'+String(Math.floor(s/60)%60).padStart(2,'0')"
"+':'+String(s%60).padStart(2,'0');"
"h+=row('Status','<span class=val>'+(c.phase==1?'Cleaning':c.phase==2?"
"'Cooling down':'Idle')+'</span>');"
"if(c.phase==1||c.phase==2)h+=row(c.phase==1?'Cleaning time left':"
"'Cooldown left','<span class=val id=cleanleft data-s=\"'+c.remain+'\">'+"
"hms(c.remain)+'</span>');"
"h+='<div class=none>Heats the temperature/humidity sensor for two hours "
"to dry it out and remove moisture and contamination. Stopping it, or the "
"end of the two hours, starts a five-minute cooldown before its readings "
"count again; the switch is already off during the cooldown. Both timers "
"run on the controller and match the app. Needs controller firmware 3.0 "
"or newer.</div>';"
"h+='</fieldset>';}"
// --- Alarm settings ---
//
// Each range alarm: on/off switch, then the minimum and maximum as selects
// over the allowed range; PPFD has only a maximum. The plain alarms are
// switches. Every change is sent at once, as the whole alarm block (the
// firmware fills in the rest from what the controller reported).
"if(D.alarm&&D.alarm_spec){"
"const A=D.alarm,P=D.alarm_spec;"
"h+='<fieldset><legend>Alarms</legend>';"
"const am=(k,w,v)=>send('alarm',k,w+'='+v);"
"for(const r of P.r){"
"const g=A[r.k]||{};const on=g.enabled==1;"
"let c='<button class=\"'+(on?'on':'off')+'\" onclick=\"send(\\'alarm\\',\\''+r.k+"
"'\\',\\'enabled='+(on?'OFF':'ON')+'\\')\">'+(on?'ON':'OFF')+'</button>';"
"const dp=r.st<1?1:0;"
"const mk=(lo,hi)=>{const o=[];for(let i=0;i<=Math.round((hi-lo)/r.st);i++)o.push((lo+i*r.st).toFixed(dp));return o;};"
// The current value is kept selectable even when it lies outside the range,
// so a value set elsewhere is shown rather than silently replaced.
"const pick=(w,cur,lo,hi)=>{const opts=mk(lo,hi);const cv=(cur===undefined)?'':Number(cur).toFixed(dp);"
"if(cv&&opts.indexOf(cv)<0){opts.push(cv);opts.sort((a,b)=>a-b);}"
"return now(cv)+'<select onchange=\"send(\\'alarm\\',\\''+r.k+'\\',\\''+w+'=\\'+this.value)\">'+"
"opts.map(o=>'<option'+(o===cv?' selected':'')+'>'+o+'</option>').join('')+'</select>';};"
"if(r.mn)c+=' <span class=tgt>Min</span>'+pick('min',g.vmin,r.nlo,r.nhi);"
"c+=' <span class=tgt>Max</span>'+pick('max',g.vmax,r.xlo,r.xhi)+' <span class=none>'+esc(r.u)+'</span>';"
"h+=row(esc(r.l),c);}"
"for(const s of P.s){"
"if(A[s.k]===undefined)continue;"
"const on=A[s.k]==1;"
"h+=row(esc(s.l),'<button class=\"'+(on?'on':'off')+'\" onclick=\"send(\\'alarm\\',\\''+s.k+"
"'\\',\\''+(on?'OFF':'ON')+'\\')\">'+(on?'ON':'OFF')+'</button>');}"
"h+='<div class=none>The controller raises these alarms itself, so they also "
"reach the Spider Farmer app. Changes here are sent to the controller at "
"once.</div>';"
"h+='</fieldset>';}"
// --- Sensor calibration ---
//
// Applied by the controller itself, so corrected readings reach the vendor
// app and cloud as well as Home Assistant.
"if(D.cal){"
"h+='<fieldset><legend>Sensor calibration</legend>';"
// Stepped selects, matching the ranges and steps Home Assistant is given
// by tools/gen_discovery.py: both sides offer the same values, so anything
// chosen in one is always selectable in the other.
//
// Every offset here moves in tenths (CO2 in tens of ppm), which is finer
// than the old coarse step lists could express and finer than the sensor
// drift the offsets exist to correct, so a plain select of every value
// stays a usable list rather than an unusable slider.
"const CU=[['temp','Temperature','\\u00b0C',-10,10,0.1],"
"['humi','Humidity','%',-20,20,0.1],"
"['co2','CO\\u2082','ppm',-200,200,10],"
"['ppfd','PPFD','',-20,20,0.1]];"
"for(const[k,nm,u,lo,hi,stp] of CU){"
// Rounded to two decimals because the value arrives as a float: 0.2 comes
// back as 0.20000001788139343, which would match no option.
"const v=Math.round((D.cal[k]||0)*100)/100;"
"h+=row(nm+(u?' ('+u+')':''),selRange('calibration',k,v,lo,hi,stp));}"
"h+='<div class=none>Offsets are added to the controller\\u0027s own "
"readings, so corrected values also appear in the Spider Farmer app. "
"The value on the left is what the controller currently has.</div>';"
"h+='</fieldset>';}"
// --- Day cycle targets ---
//
// The thresholds the environment modes steer towards, as one flat block
// the controller expects to be written whole. Times are seconds since
// midnight on the wire and HH:MM here, so they are converted on the way in.
//
// The buttons carry a data attribute rather than an inline onclick. Every
// other control in this file uses onclick, but those are static strings;
// these need a variable substituted into a nested quoted argument, which
// means escaping quotes twice over inside a C string literal. Binding by
// data attribute avoids that and keeps the markup readable.
"if(D.target){"
"const T=D.target,dt=T.dayTime||{};"
"const hhmm=function(s){if(typeof s!=='number')return '';"
"const m=((s%86400)+86400)%86400;"
"return String(Math.floor(m/3600)).padStart(2,'0')+':'+"
"String(Math.floor((m%3600)/60)).padStart(2,'0');};"
"h+='<fieldset><legend>Day cycle targets</legend>';"
// The same clock as everywhere else. These two arrive as seconds since
// midnight, so they are converted to HH:MM first -- the clock speaks the
// page's format, not the controller's.
"h+=row('Day starts',clock('target','day_time_start',hhmm(dt.startTime),0));"
"h+=row('Day ends',clock('target','day_time_end',hhmm(dt.endTime),0));"
"h+='<div class=none>Between those two times the day targets apply; "
"outside them the night targets do.</div>';"
// Ranges match the firmware's, so a value the box refuses is one the
// controller would clamp anyway. Everything is a select over the allowed
// values, and the step is wide enough to keep the list usable: a
// temperature list in 0.1 steps would be five hundred entries.
//
// These do not send on change. The controller takes the target block only
// as a whole, so the three values of a group are collected and submitted
// together by the Set button beside them.
// [key, label, unit, target lo, target hi, target step,
//  deadband lo, deadband hi, deadband step] -- as the app offers them:
// targets 0-50 / 0-100 in 1s and 0-2500 ppm in 10s; deadbands 1-10 in 1s
// for temperature and humidity, 10-250 ppm in 10s for CO2.
"const TG=[['temp','Temperature','\\u00b0C',0,50,1,1,10,1],"
"['humi','Humidity','%',0,100,1,1,10,1],"
"['co2','CO\\u2082','ppm',0,2500,10,10,250,10]];"
"for(const g of TG){"
"const b=T[g[0]]||{};"
"h+='<div class=r><span class=lbl>'+g[1]+'</span>';"
"const cols=[['target_day','Day',g[3],g[4],g[5]],"
"['target_night','Night',g[3],g[4],g[5]],"
"['deadband','Deadband',g[6],g[7],g[8]]];"
"for(const c of cols){"
"h+='<span class=tgt>'+c[1]+'</span>';"
// The deadband starts where its sensor's value can usefully change, so it
// is offered from the bottom of the range rather than from zero.
"h+=pickNum(b[c[0]],c[2],c[3],c[4],'t_'+g[0]+'_'+c[0])+' ';} "
"h+='<button data-grp='+g[0]+'>Set</button></div>';}"
"h+='</fieldset>';}"
// --- Grow plan ---
"h+=planBlock(D);"
// --- Time zone ---
"h+='<fieldset><legend>Time zone</legend>';"
"h+=row('Managed by','<span class=val>'+(S.tz_push?"
"'Bridge Settings (central)':'This controller (local)')+'</span>');"
"h+=row('Reported by the controller','<span class=val>'+"
"esc(D.tz_name||'unknown')+'</span>');"
// Same as the app's "Sync device time": zone and the bridge's UTC clock.
"h+='<div class=r><span class=lbl>Device time</span><button onclick=\"send(\\'time_sync\\',\\'\\',\\'PRESS\\')\">"
"Sync device time</button><span class=val>'+(S.tz_push?'also done on connect and every 6 h':'sends the bridge clock now')+'</span></div>';"
// The daylight-saving state THIS controller is actually running,
// derived from the rules it reports in tz_posix -- not the bridge's own
// setting, which is a different value and was shown here by mistake.
//
// A TZ string with switching rules (a comma followed by M-dates) means
// automatic; one without means the clock is pinned, and whether that
// pin is summer or standard follows from whether the name carries a
// second, 'summer' zone abbreviation.
"const tzr=D.tz_posix||'';"
"const auto=tzr.indexOf(',')>=0;"
"const dsw2=/^[A-Za-z]{2,5}[+-]?[\\d:]*[A-Za-z]{2,5}/.test(tzr)&&!auto;"
"h+='<div class=\"r\"><span class=lbl>Summer time</span>'+"
"'<label class=sw><input type=checkbox id=dsw '+(dsw2?'checked ':'')+"
"(auto?'disabled ':'')+"
"'onchange=\"setDsw()\"><span></span></label>'+"
"'<span class=val>'+(auto?'decided by the zone rules':"
"(dsw2?'forced on':'forced off'))+'</span></div>';"
"h+='<div class=\"r\"><span class=lbl>Follow the zone rules</span>'+"
"'<label class=sw><input type=checkbox id=dau '+(auto?'checked ':'')+"
"'onchange=\"setDau()\"><span></span>"
"</label><span class=val>automatic</span></div>';"
"if(S.tz_push){"
"h+='<div class=none>The bridge manages the clock for every "
"controller, so these follow its settings. Turn off \\u0022Apply this "
"to every controller\\u0022 in Settings to change them here.</div>';}"
"else{"
"h+=row('Set zone to','<select id=tzsel data-own=1></select> "
"<button onclick=\"setTz()\">Apply</button>');"
"h+='<div class=none>Schedule times on the controller are local, so "
"this decides when they run.</div>';}"
"h+='</fieldset>';"
// --- What the controller reports about itself ---
//
// Raw counters are converted where a bare number means nothing: uptime
// as days and hours, the update epoch as a date.
"if(D.sys){"
"const y=D.sys;"
"h+='<fieldset><legend>Controller</legend>';"
"if(y.localtime)h+=row('Its clock','<span class=val>'+esc(y.localtime)+"
"'</span>');"
"if(y.timezone)h+=row('Time zone','<span class=val>'+esc(y.timezone)+"
"'</span>');"
// The raw rules plus what they mean: whether summer time is switched by
// the rules, pinned on or pinned off, and whether the zone is set here or
// pushed by the bridge from Settings.
"if(y.TZ){"
"const yr=String(y.TZ);"
"const yAuto=yr.indexOf(',')>=0;"
"const ySum=!yAuto&&/^[A-Za-z]{2,5}[+-]?[\\d:]*[A-Za-z]{2,5}/.test(yr);"
"h+=row('Daylight saving rules','<span class=val>'+esc(yr)+'</span>');"
"h+=row('Summer time switch','<span class=val>'+"
"(yAuto?'Automatic (follows the zone rules)':"
"(ySum?'On (summer time forced)':'Off (standard time forced)'))+"
"'</span>');"
"h+=row('Time zone managed by','<span class=val>'+"
"(S.tz_push?'Bridge Settings \\u2014 applied to every controller, not "
"changeable here':'This controller \\u2014 change it under Time zone above')+"
"'</span>');}"
"if(y.tzoff!==undefined)h+=row('Offset from UTC','<span class=val>'+"
"(y.tzoff>=0?'+':'')+(y.tzoff/3600)+' h</span>');"
"if(y.ver)h+=row('Firmware','<span class=val>'+esc(y.ver)+'</span>');"
"if(y.hwver)h+=row('Hardware','<span class=val>'+esc(y.hwver)+"
"(y.hwcode!==undefined?' (code '+y.hwcode+')':'')+'</span>');"
"if(y.buildTime)h+=row('Firmware built','<span class=val>'+"
"esc(y.buildTime)+'</span>');"
"if(y.verUpdateTime)h+=row('Firmware updated','<span class=val>'+"
"new Date(y.verUpdateTime*1000).toLocaleString()+"
"(y.verUpdateWho?' by '+esc(y.verUpdateWho):'')+"
"(y.verUpdateNum!==undefined?' (#'+y.verUpdateNum+')':'')+'</span>');"
"if(y.upTime!==undefined)h+=row('Uptime','<span class=val>'+"
"dur(y.upTime)+'</span>');"
"if(y.upCount!==undefined)h+=row('Restarts','<span class=val>'+"
"y.upCount+'</span>');"
"if(y.mem!==undefined)h+=row('Free memory','<span class=val>'+y.mem+"
"'</span>');"
"if(y.wifi){"
"if(y.wifi.rssi!==undefined)h+=row('Signal','<span class=val>'+"
"y.wifi.rssi+' dBm</span>');"
"if(y.wifi.isConnect!==undefined)h+=row('WiFi','<span class=val>'+"
"(y.wifi.isConnect?'connected':'disconnected')+'</span>');}"
"h+='</fieldset>';}"
"morphHTML(a,h);"
"bindTargetButtons();"
"if(!S.tz_push)fillZones(D.tz_name);}"
// ---------------------------------------------------------------------------
// Grow plan
//
// plan = {enabled, stage:[{stageId, label, startDate, endDate, alarmDate,
// color, light1, light2, target}]}. Dates are packed (year<<16|month<<8|
// day); alarmDate is epoch seconds; times are seconds since midnight. Each
// stage is saved on its own (a=stage), never the whole plan; the bridge
// keeps the rest and writes the plan to the controller. At most
// S.plan_max stages (SB_PLAN_MAX_STAGES).
// Templates (5 system, up to 50 custom) come from the bridge (/control/templates).
// PE holds the stage or template being edited; PEK says which:
// 'stage' or a template id ("cust:N").
// ---------------------------------------------------------------------------
"let PE=null,PEnew=false,PEK='stage',TPLS=null,PESEQ=0,TPLT=0,TPLBUSY=false;"
// The most stages the controller takes, as the bridge enforces it.
"const PMAXD=5;function pmax(){return (S&&S.plan_max)||PMAXD;}"
// The five colours the controller knows (1-5), one per system template.
"const PCOL=[['1','Green','#22c55e'],['2','Blue','#3b82f6'],"
"['3','Purple','#a855f7'],['4','Orange','#f97316'],['5','Red','#ef4444']];"
"const pd2s=p=>p?(p>>16)+'-'+String((p>>8)&255).padStart(2,'0')+'-'+String(p&255).padStart(2,'0'):'';"
"const s2pd=s=>{const m=/^(\\d{4})-(\\d{2})-(\\d{2})$/.exec(s||'');return m?((+m[1])<<16)|((+m[2])<<8)|(+m[3]):0;};"
"const pdDays=(a,b)=>{const d=p=>Date.UTC(p>>16,((p>>8)&255)-1,p&255);return Math.round((d(b)-d(a))/86400000);};"
"const pcol=c=>(PCOL.find(x=>+x[0]===+c)||PCOL[0]);"
// One request at a time; a busy (503) or failed load is retried after 3 s
// by a single timer, never in a render loop.
"async function loadTpls(){if(TPLBUSY)return;TPLBUSY=true;"
"try{const r=await fetch('/control/templates');if(!r.ok)throw 0;TPLS=await r.json();}"
"catch(e){clearTimeout(TPLT);TPLT=setTimeout(()=>{TPLT=0;loadTpls().then(()=>{if(TPLS)render();});},3000);}"
"finally{TPLBUSY=false;}}"
"function tplOptions(){if(!TPLS)return '<option value=sys:seedling>Seedling</option>';"
"let o='<optgroup label=\"System\">';for(const t of TPLS.system)o+='<option value='+t.id+'>'+esc(t.tpl.name)+'</option>';"
"o+='</optgroup>';const c=TPLS.custom.filter(t=>t.tpl);if(c.length){o+='<optgroup label=\"Custom\">';"
"for(const t of c)o+='<option value='+t.id+'>'+esc(t.tpl.name)+'</option>';o+='</optgroup>';}return o;}"
"function planBlock(D){"
"const P=D.plan;let h='<fieldset><legend>Grow plan</legend>';"
// The plan lives on the controller only; there is no bridge-side plan.
"if(!P){h+='<div class=none>The controller has not reported its plan yet. Stages can be added once it has.</div></fieldset>'+tplBlock();return h;}"
"const on=!!P.enabled,st=P.stage||[];"
"h+='<div class=r><span class=lbl>Plan</span><label class=sw><input type=checkbox '+(on?'checked ':'')+"
"'onchange=\"planEnable(this.checked,this)\"><span></span></label><span class=val>'+(on?'running':'stopped')+"
"' &middot; '+st.length+' / '+pmax()+' stages</span></div>';"
"if(!st.length)h+='<div class=none>No stages. Add one from a template below.</div>';"
"for(const s of st){const c=pcol(s.color);const cur=D.today&&D.today>=s.startDate&&D.today<=s.endDate;"
"const days=pdDays(s.startDate,s.endDate)+1;"
"h+='<div class=r style=\"border-left:4px solid '+c[2]+';padding-left:.5rem\"><span class=lbl><b>'+esc(s.label||'Stage')+'</b>'"
"+(cur?' <span class=val style=\"color:#4ade80\">today: day '+(pdDays(s.startDate,D.today)+1)+' of '+days+'</span>':'')+'</span>'"
"+'<span class=val>'+pd2s(s.startDate)+' &rarr; '+pd2s(s.endDate)+' ('+days+' d)'"
"+(s.alarmDate?' &middot; reminder '+new Date(s.alarmDate*1000).toLocaleString():'')"
"+' &middot; L1 '+planLightSummary(s.light1)+' &middot; L2 '+planLightSummary(s.light2)"
"+(s.target&&s.target.temp?' &middot; '+s.target.temp.targetDay+'/'+s.target.temp.targetNight+'\\u00b0C':'')"
"+(s.target&&s.target.humi?' '+s.target.humi.targetDay+'/'+s.target.humi.targetNight+'%':'')"
"+(s.target&&s.target.co2?' '+s.target.co2.targetDay+'/'+s.target.co2.targetNight+'ppm':'')+'</span>'"
"+'<span><button onclick=\"planEdit('+s.stageId+')\">Edit</button> '"
"+'<button onclick=\"planToTpl('+s.stageId+')\">Save as template</button> '"
"+'<button class=danger onclick=\"planDelete('+s.stageId+',\\''+esc(s.label||'').replace(/\\x27/g,'')+'\\')\">Delete</button></span></div>';}"
"if(st.length<pmax())h+='<div class=r><span class=lbl>Add stage</span><span><select id=planTplSel>'+tplOptions()+'</select> '"
"+'<button onclick=\"planAddTpl()\">Add from template</button> '"
"+'<button onclick=\"planNewFromTpl()\">Add and edit first</button></span></div>';"
"else h+='<div class=none>The plan has '+pmax()+' stages, the most a controller takes.</div>';"
"if(PE&&PEK==='stage')h+=planEditor(PE);"
"h+='<div class=none>Same as the app: each stage carries its own light settings for both lights and the "
"environment targets, and applies between its start and end date while the plan is running.</div>';"
"return h+'</fieldset>'+tplBlock();}"
// --- Templates ---
"function tplBlock(){let h='<fieldset><legend>Plan templates</legend>';"
"if(!TPLS){if(!TPLBUSY&&!TPLT)loadTpls().then(()=>{if(TPLS)render();});return h+'<div class=none>Loading&hellip;</div></fieldset>';}"
"h+='<div class=none>System templates are fixed; clone one to make your own. Up to '+(TPLS.max||50)+' custom templates, '"
"+'stored in the bridge\\'s flash.</div>';"
"for(const t of TPLS.system){h+='<div class=r><span class=lbl>'+esc(t.tpl.name)+' <small>(system, '+t.tpl.days+' d)</small></span>'"
"+'<span><button onclick=\"tplClone(\\''+t.id+'\\')\">Clone to custom</button></span></div>';}"
"for(const t of TPLS.custom){if(!t.tpl){continue;}"
"h+='<div class=r style=\"border-left:4px solid '+pcol(t.tpl.color)[2]+';padding-left:.5rem\"><span class=lbl>'+esc(t.tpl.name)+' <small>('+t.tpl.days+' d)</small></span>'"
"+'<span><button onclick=\"tplEdit(\\''+t.id+'\\')\">Edit</button> <button onclick=\"tplClone(\\''+t.id+'\\')\">Clone</button> '"
"+'<button class=danger onclick=\"tplDelete(\\''+t.id+'\\')\">Delete</button></span></div>';}"
"const used=TPLS.custom.filter(t=>t.tpl).length;"
"h+='<div class=none>'+used+' / '+(TPLS.max||50)+' custom templates.</div>';"
"if(PE&&PEK!=='stage')h+=planEditor(PE);"
"return h+'</fieldset>';}"
"function tplFind(id){if(!TPLS)return null;return TPLS.system.concat(TPLS.custom).find(t=>t.id===id);}"
"async function tplPost(body){try{const r=await fetch('/control/template',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});const t=await r.text();"
"toast(r.ok?t:'Failed: '+t);await loadTpls();render();return r.ok?t:null;}catch(e){toast('Connection lost');return null;}}"
"async function tplClone(id){const t=tplFind(id);if(!t)return;"
"const n=prompt('Name of the new template',t.tpl.name+' (Copy)');if(n===null)return;"
"await tplPost('a=clone&src='+id+'&name='+encodeURIComponent(n));}"
"async function tplDelete(id){const t=tplFind(id);if(!t||!confirm('Delete template \"'+t.tpl.name+'\"?'))return;"
"await tplPost('a=delete&id='+id);}"
"function tplEdit(id){const t=tplFind(id);if(!t)return;PE=JSON.parse(JSON.stringify(t.tpl));"
"PE.label=PE.name;PEK=id;PEnew=false;render();"
"setTimeout(()=>{const e=document.getElementById('pe');if(e)e.scrollIntoView({behavior:'smooth'});},50);}"
"async function planToTpl(sid){const s=(dev().plan.stage||[]).find(x=>x.stageId===sid);if(!s)return;"
"if(!TPLS||!TPLS.free){toast('All '+((TPLS&&TPLS.max)||50)+' template slots are used -- delete one first');return;}"
"const n=prompt('Template name',s.label);if(n===null)return;"
"const t={name:n,days:pdDays(s.startDate,s.endDate)+1,color:s.color,light1:s.light1,light2:s.light2,target:s.target};"
"await tplPost('a=save&id='+TPLS.free+'&name='+encodeURIComponent(n)+'&v='+encodeURIComponent(JSON.stringify(t)));}"
// --- Stages ---
"function D0(){const d=new Date();return (d.getFullYear()<<16)|((d.getMonth()+1)<<8)|d.getDate();}"
"function addDays(p,n){const d=new Date(p>>16,((p>>8)&255)-1,(p&255)+n);return (d.getFullYear()<<16)|((d.getMonth()+1)<<8)|d.getDate();}"
"function nextStart(){const st=(dev().plan&&dev().plan.stage)||[];"
"let e=0;for(const s of st)if(s.endDate>e)e=s.endDate;return e?addDays(e,1):D0();}"
"async function planAddTpl(){const id=gv('planTplSel');await planPost('template',id);}"
"function planNewFromTpl(){const id=gv('planTplSel');const t=tplFind(id);if(!t)return;"
"const s=JSON.parse(JSON.stringify(t.tpl));const start=nextStart();"
"PE={stageId:Math.floor(Date.now()/1000),label:s.name,startDate:start,endDate:addDays(start,(s.days||14)-1),alarmDate:0,"
"color:s.color,light1:s.light1,light2:s.light2,target:s.target};PEK='stage';PEnew=true;render();"
"setTimeout(()=>{const e=document.getElementById('pe');if(e)e.scrollIntoView({behavior:'smooth'});},50);}"
"function planEdit(id){const P=dev().plan;const s=(P.stage||[]).find(x=>x.stageId===id);if(!s)return;"
"PE=JSON.parse(JSON.stringify(s));PEK='stage';PEnew=false;render();"
"setTimeout(()=>{const e=document.getElementById('pe');if(e)e.scrollIntoView({behavior:'smooth'});},50);}"
"function planCancel(){PE=null;render();}"
"function planLightSummary(l){if(!l)return '-';if(l.modeType===12){const p=(l.ppfdPeriod||[])[0]||{};"
"return 'PPFD '+(p.brightness||0)+' '+hhmm(p.startTime)+'-'+hhmm(p.endTime);}"
"if(l.modeType===1){const p=(l.timePeriod||[])[0]||{};return 'Schedule '+(p.brightness||0)+'% '+hhmm(p.startTime)+'-'+hhmm(p.endTime);}"
"return 'Manual '+(l.mOnOff?(l.mLevel||0)+'%':'off');}"
"const hhmm=s=>s===undefined?'--:--':String(Math.floor(s/3600)).padStart(2,'0')+':'+String(Math.floor(s%3600/60)).padStart(2,'0');"
// Select helpers: every numeric field is a select, as elsewhere on the page.
// The current value is always offered even when off the step grid.
"function psel(id,from,to,step,val,fmt){let o='',have=false;for(let v=from;v<=to+1e-9;v=Math.round((v+step)*1000)/1000){"
"const sel=Math.abs(v-val)<1e-9;if(sel)have=true;o+='<option value=\"'+v+'\"'+(sel?' selected':'')+'>'+(fmt?fmt(v):v)+'</option>';}"
"if(!have&&val!==undefined&&val!==null)o='<option value=\"'+val+'\" selected>'+(fmt?fmt(val):val)+'</option>'+o;"
"return '<select id='+id+'>'+o+'</select>';}"
// Times as hour and minute selects, to the minute -- the same picker the
// device forms use (clock()), instead of a 15-minute list.
"function tsel(id,secs){secs=secs||0;const H=Math.floor(secs/3600)%24,M=Math.floor(secs%3600/60);"
"const pad=n=>String(n).padStart(2,'0');let a='',b='';"
"for(let i=0;i<24;i++)a+='<option value='+i+(i===H?' selected':'')+'>'+pad(i)+'</option>';"
"for(let i=0;i<60;i++)b+='<option value='+i+(i===M?' selected':'')+'>'+pad(i)+'</option>';"
"return '<select id='+id+'_h>'+a+'</select>:<select id='+id+'_m>'+b+'</select>';}"
"const gt=id=>(+document.getElementById(id+'_h').value)*3600+(+document.getElementById(id+'_m').value)*60;"
// Off or a value in lo..hi, like the device forms' threshold and fade
// selects (Off is the controller's 0).
"function pselOff(id,lo,hi,step,val,fmt){val=+val||0;let o='<option value=\"0\"'+(val?'':' selected')+'>Off</option>',have=!val;"
"for(let v=lo;v<=hi+1e-9;v=Math.round((v+step)*1000)/1000){const s=val&&Math.abs(v-val)<1e-9;if(s)have=true;"
"o+='<option value=\"'+v+'\"'+(s?' selected':'')+'>'+(fmt?fmt(v):v)+'</option>';}"
"if(!have)o+='<option value=\"'+val+'\" selected>'+(fmt?fmt(val):val)+'</option>';"
"return '<select id='+id+'>'+o+'</select>';}"
// The editor: every field of a stage (or template). Light blocks follow
// the app's three modes; the fields shown depend on the mode.
// data-frozen + data-k: once open, the editor is left as the user is
// filling it in; another stage or template gets a new key, so it is
// rebuilt then.
// Not enumerable, so it never ends up in the JSON sent to the bridge.
"function planEditor(s){const isT=PEK!=='stage';if(!s._k)Object.defineProperty(s,'_k',{value:++PESEQ,enumerable:false});"
"let h='<div id=pe data-frozen=1 data-k=\"pe'+s._k+'\" style=\"border:1px solid #60a5fa;border-radius:8px;padding:.6rem;margin:.6rem 0\">';"
"h+='<b>'+(isT?'Edit template':(PEnew?'New stage':'Edit stage'))+'</b>';"
"h+=prow('Name','<input id=pe_label type=text maxlength=30 value=\"'+esc(s.label||'')+'\">');"
"if(isT){h+=prow('Length',psel('pe_days',1,120,1,s.days||14,v=>v+' days'));}"
"else{h+=prow('Start date','<input id=pe_start type=date value=\"'+pd2s(s.startDate)+'\">');"
"h+=prow('End date','<input id=pe_end type=date value=\"'+pd2s(s.endDate)+'\">');"
"const al=s.alarmDate?new Date(s.alarmDate*1000):null;"
"const alv=al?al.getFullYear()+'-'+String(al.getMonth()+1).padStart(2,'0')+'-'+String(al.getDate()).padStart(2,'0')+'T'+String(al.getHours()).padStart(2,'0')+':'+String(al.getMinutes()).padStart(2,'0'):'';"
"h+=prow('Reminder (end of stage)','<input id=pe_alarm type=datetime-local value=\"'+alv+'\"> <small>empty = none</small>');}"
"h+=prow('Stage colour','<select id=pe_color>'+PCOL.map(c=>'<option value='+c[0]+(+c[0]===+(s.color||1)?' selected':'')+' style=\"color:'+c[2]+'\">'+c[1]+'</option>').join('')+'</select>');"
"h+=planLightEditor('l1','Light 1',s.light1||{});h+=planLightEditor('l2','Light 2',s.light2||{});"
"h+=planTargetEditor(s.target||{});"
"h+='<div class=r style=\"margin-top:.6rem\"><button onclick=\"planSave()\">'+(isT?'Save template':'Save stage to controller')+'</button> '"
"+'<button onclick=\"planCancel()\">Cancel</button></div></div>';return h;}"
"function prow(l,c){return '<div class=r><span class=lbl>'+l+'</span><span>'+c+'</span></div>';}"
"function planLightEditor(k,title,l){const m=l.modeType===undefined?12:l.modeType;"
"const tp=(l.timePeriod||[])[0]||{},pp=(l.ppfdPeriod||[])[0]||{};"
"let h='<div style=\"margin-top:.5rem;border-top:1px solid #374151;padding-top:.3rem\"><b>'+title+'</b>';"
"h+=prow('Mode','<select id=pe_'+k+'_mode onchange=\"planModeChanged(\\''+k+'\\')\">'"
"+'<option value=0'+(m===0?' selected':'')+'>Manual</option><option value=1'+(m===1?' selected':'')+'>Schedule</option>'"
"+'<option value=12'+(m===12?' selected':'')+'>PPFD (sensor)</option></select>');"
"h+='<div id=pe_'+k+'_man style=\"display:'+(m===0?'':'none')+'\">';"
"h+=prow('On','<label class=sw><input type=checkbox id=pe_'+k+'_on'+(l.mOnOff?' checked':'')+'><span></span></label>');"
"h+=prow('Brightness',psel('pe_'+k+'_lvl',11,100,1,l.mLevel||11,v=>v+' %'));h+='</div>';"
"h+='<div id=pe_'+k+'_sch style=\"display:'+(m===1?'':'none')+'\">';"
"h+=prow('Start',tsel('pe_'+k+'_ts',tp.startTime===undefined?18000:tp.startTime));"
"h+=prow('End',tsel('pe_'+k+'_te',tp.endTime===undefined?82800:tp.endTime));"
"h+=prow('Brightness',psel('pe_'+k+'_tb',11,100,1,tp.brightness||60,v=>v+' %'));"
"h+=prow('Sunrise/sunset',pselOff('pe_'+k+'_tf',1,60,1,Math.round((tp.fadeTime||0)/60),v=>v+' min'));h+='</div>';"
"h+='<div id=pe_'+k+'_ppfd style=\"display:'+(m===12?'':'none')+'\">';"
"h+=prow('Start',tsel('pe_'+k+'_ps',pp.startTime===undefined?18000:pp.startTime));"
"h+=prow('End',tsel('pe_'+k+'_pe',pp.endTime===undefined?82800:pp.endTime));"
"h+=prow('PPFD target',psel('pe_'+k+'_pt',20,2000,10,pp.brightness||300,v=>v+' \\u00b5mol'));"
"h+=prow('Sunrise/sunset',pselOff('pe_'+k+'_pf',1,60,1,Math.round((pp.fadeTime===undefined?1800:pp.fadeTime)/60),v=>v+' min'));"
"h+=prow('Min brightness',psel('pe_'+k+'_pmin',11,100,1,l.ppfdMinBrightness||11,v=>v+' %'));"
"h+=prow('Max brightness',psel('pe_'+k+'_pmax',11,100,1,l.ppfdMaxBrightness||100,v=>v+' %'));h+='</div>';"
"h+='<div id=pe_'+k+'_auto style=\"display:'+(m===0?'none':'')+'\">';"
"h+=prow('Dim threshold',pselOff('pe_'+k+'_dark',15,50,1,l.darkTemp||0,v=>v+'\\u00b0C'));"
"h+=prow('Off threshold',pselOff('pe_'+k+'_off',15,50,1,l.offTemp||0,v=>v+'\\u00b0C'));h+='</div>';"
"return h+'</div>';}"
"function planModeChanged(k){const m=+document.getElementById('pe_'+k+'_mode').value;"
"document.getElementById('pe_'+k+'_man').style.display=m===0?'':'none';"
"document.getElementById('pe_'+k+'_sch').style.display=m===1?'':'none';"
"document.getElementById('pe_'+k+'_ppfd').style.display=m===12?'':'none';"
"document.getElementById('pe_'+k+'_auto').style.display=m===0?'none':'';}"
"function planTargetEditor(t){const dt=t.dayTime||{},g=(n,d)=>t[n]||d;"
"let h='<div style=\"margin-top:.5rem;border-top:1px solid #374151;padding-top:.3rem\"><b>Environment</b>';"
"h+=prow('Day starts',tsel('pe_t_ds',dt.startTime===undefined?18000:dt.startTime));"
"h+=prow('Day ends',tsel('pe_t_de',dt.endTime===undefined?82800:dt.endTime));"
"const T=g('temp',{targetDay:23,targetNight:23,deadband:3}),H=g('humi',{targetDay:70,targetNight:70,deadband:5}),"
"C=g('co2',{targetDay:600,targetNight:400,deadband:200});"
// Same ranges and steps as the Day cycle targets form (as the app offers
// them): targets 0-50 °C / 0-100 % in 1s and 0-2500 ppm in 10s; deadbands
// 1-10 in 1s for temperature and humidity, 10-250 ppm in 10s for CO2.
"h+=prow('Temperature','Day '+psel('pe_t_temp_d',0,50,1,T.targetDay,v=>v+'\\u00b0C')+' Night '+psel('pe_t_temp_n',0,50,1,T.targetNight,v=>v+'\\u00b0C')+' Deadband '+psel('pe_t_temp_b',1,10,1,T.deadband,v=>v+'\\u00b0C'));"
"h+=prow('Humidity','Day '+psel('pe_t_humi_d',0,100,1,H.targetDay,v=>v+' %')+' Night '+psel('pe_t_humi_n',0,100,1,H.targetNight,v=>v+' %')+' Deadband '+psel('pe_t_humi_b',1,10,1,H.deadband,v=>v+' %'));"
"h+=prow('CO\\u2082','Day '+psel('pe_t_co2_d',0,2500,10,C.targetDay,v=>v+' ppm')+' Night '+psel('pe_t_co2_n',0,2500,10,C.targetNight,v=>v+' ppm')+' Deadband '+psel('pe_t_co2_b',10,250,10,C.deadband,v=>v+' ppm'));"
"return h+'</div>';}"
"const gv=id=>document.getElementById(id).value;const gn=id=>+gv(id);"
"function planReadLight(k){const m=gn('pe_'+k+'_mode');"
"return {modeType:m,darkTemp:gn('pe_'+k+'_dark'),offTemp:gn('pe_'+k+'_off'),"
"timePeriod:[{enabled:1,weekmask:127,startTime:gt('pe_'+k+'_ts'),endTime:gt('pe_'+k+'_te'),brightness:gn('pe_'+k+'_tb'),fadeTime:gn('pe_'+k+'_tf')*60}],"
"ppfdPeriod:[{enabled:1,weekmask:127,startTime:gt('pe_'+k+'_ps'),endTime:gt('pe_'+k+'_pe'),brightness:gn('pe_'+k+'_pt'),fadeTime:gn('pe_'+k+'_pf')*60}],"
"mOnOff:document.getElementById('pe_'+k+'_on').checked?1:0,mLevel:gn('pe_'+k+'_lvl'),"
"ppfdMinBrightness:gn('pe_'+k+'_pmin'),ppfdMaxBrightness:gn('pe_'+k+'_pmax')};}"
"function planReadCommon(s){s.label=gv('pe_label').trim()||'Stage';s.color=gn('pe_color');"
"s.light1=planReadLight('l1');s.light2=planReadLight('l2');"
"const tg=k=>({targetDay:gn('pe_t_'+k+'_d'),targetNight:gn('pe_t_'+k+'_n'),deadband:gn('pe_t_'+k+'_b')});"
"s.target={dayTime:{startTime:gt('pe_t_ds'),endTime:gt('pe_t_de')},temp:tg('temp'),humi:tg('humi'),co2:tg('co2')};}"
"async function planSave(){const s=PE;if(!s)return;planReadCommon(s);"
"if(PEK!=='stage'){const t={name:s.label,days:gn('pe_days'),color:s.color,light1:s.light1,light2:s.light2,target:s.target};"
"if(await tplPost('a=save&id='+PEK+'&name='+encodeURIComponent(s.label)+'&v='+encodeURIComponent(JSON.stringify(t)))!==null){PE=null;render();}return;}"
"s.startDate=s2pd(gv('pe_start'));s.endDate=s2pd(gv('pe_end'));"
"if(!s.startDate||!s.endDate){toast('Start and end date are required');return;}"
"if(s.endDate<s.startDate){toast('End date is before the start date');return;}"
"const av=gv('pe_alarm');s.alarmDate=av?Math.floor(new Date(av).getTime()/1000):0;"
"const st=(dev().plan&&dev().plan.stage)||[];"
"for(const o of st){if(o.stageId!==s.stageId&&!(s.endDate<o.startDate||s.startDate>o.endDate)){toast('Overlaps the stage \"'+o.label+'\"');return;}}"
"if(await planPost('stage',JSON.stringify(s)))PE=null;render();}"
"async function planDelete(id,label){if(!confirm('Delete stage \"'+label+'\" from the plan on the controller?'))return;"
"await planPost('delete',String(id));}"
// Cancel puts the switch back at once: the hold the change set is lifted.
"function planEnable(on,el){if(!on&&!confirm('Stop the plan? The controller then keeps its current light and environment settings as they are.')){"
"if(el){delete el.dataset.hold;el.checked=true;}render();return;}"
"planPost(on?'enable':'disable','');}"
"async function planPost(a,v){try{const r=await fetch('/control/plan',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'d='+SEL+'&a='+a+'&v='+encodeURIComponent(v)});"
"const t=await r.text();toast(r.ok?'Plan updated':'Failed: '+t);"
"setTimeout(checkVer,800);setTimeout(checkVer,4000);return r.ok;}catch(e){toast('Connection lost');return false;}}"
// Seconds as something readable.
"function dur(s){"
"if(s<90)return s+'s';"
"if(s<5400)return Math.floor(s/60)+'m '+(s%60)+'s';"
"if(s<172800)return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';"
"return Math.floor(s/86400)+'d '+Math.floor((s%86400)/3600)+'h';}"
// The zone list comes from the bridge so the page and the firmware
// cannot disagree about which zones exist.
"let ZONES=null;"
"async function fillZones(cur){"
"const sel=document.getElementById('tzsel');if(!sel)return;"
"if(!ZONES){try{ZONES=await (await fetch('/control/zones')).json();}"
"catch(e){return;}}"
// Filled once; afterwards only the selection follows the controller, and
// not while the user is choosing or has chosen without applying.
"if(!sel.options.length){"
"for(const z of ZONES){"
"const o=document.createElement('option');"
"o.value=z;o.textContent=z;if(z===cur)o.selected=true;"
"sel.appendChild(o);}}"
"else{if(sel.dataset.dirty&&cur===sel.value)delete sel.dataset.dirty;"
"if(!sel.dataset.dirty&&!sel.dataset.busy&&document.activeElement!==sel&&cur)sel.value=cur;}}"
// --- Day cycle target setters ---
//
// One field at a time, in the same shape the calibration selects use. The
// controller discards a partial target block, so the firmware rebuilds the
// other eleven values from its cache and sends the whole block; only the
// field being changed comes from here.
"async function setTargets(g){"
"const fl=['target_day','target_night','deadband'];"
"const out=[];"
"for(const sfx of fl){"
"const el=document.getElementById('t_'+g+'_'+sfx);"
"if(!el)continue;"
"const v=el.value.trim();"
// All three are sent because the controller writes the block whole, so a
// half-filled group would blank the field it did not carry.
"if(v===''){toast('Fill in all three values first');return;}"
"out.push([g+'_'+sfx,v]);}"
"for(const kv of out)await send('target',kv[0],kv[1]);"
// Sent: the fields follow the controller again (showing the new values
// once it has confirmed them).
"for(const sfx of fl){const el=document.getElementById('t_'+g+'_'+sfx);if(el)delete el.dataset.dirty;}}"
// Wired once per render. The buttons are rebuilt with the markup, so a
// delegated listener on the container would be the alternative; attaching
// here keeps the two in step.
"function bindTargetButtons(){"
"for(const b of document.querySelectorAll('[data-grp]'))"
"b.onclick=()=>setTargets(b.dataset.grp);}"
// After applying, the picker follows the controller again (the next
// report confirms or corrects the zone).
"async function setTz(){"
"const sel=document.getElementById('tzsel');"
"if(sel&&sel.value){await send('timezone','',sel.value);delete sel.dataset.dirty;}}"
// Per-device daylight saving. These were called but never defined,
// which made the switches silently do nothing -- a click fired a
// reference error instead of a command, so nothing was ever sent or
// saved. Reuses the same two-field command the bridge settings use:
// field \"dst\", value \"0\"/\"1\"/\"2\" for automatic/standard/summer.
"async function setDsw(){"
"const el=document.getElementById('dsw');if(!el)return;"
"await send('dst','',el.checked?'2':'1');}"
"async function setDau(){"
"const el=document.getElementById('dau');if(!el)return;"
"await send('dst','',el.checked?'0':(document.getElementById('dsw')&&"
"document.getElementById('dsw').checked?'2':'1'));}"
// Never redraw while something is being typed.
//
// render() rebuilds the whole markup, so a refresh destroys the element
// being edited and takes its value with it. Naming two specific fields
// was not enough -- the calibration inputs were cleared mid-entry, which
// made them look broken.
//
// Any focused input now defers the refresh, and a recent keystroke holds
// it off a moment longer so a redraw cannot land between two characters.
"let lastKey=0;"
"document.addEventListener('keydown',()=>{lastKey=Date.now();});"
// Only typing fields count. A select keeps the focus after a choice is
// made, and counting it froze every refresh until the user clicked
// elsewhere -- the page then kept showing the old values after a change.
// Selects, switches and buttons send at once and never need protecting.
// With the in-place merge a refresh no longer destroys what is being
// edited, so it only waits out an active keystroke burst.
"function busyEditing(){return Date.now()-lastKey<1500;}"
"let loading=false,VER=-1,again=false,retryT=0;"
// Every delayed reload goes through one timer: a new request replaces the
// pending one instead of adding another (busy replies and a polling tab
// used to stack up timers into a burst of requests).
"function later(ms){clearTimeout(retryT);retryT=setTimeout(()=>{retryT=0;load();},ms);}"
"async function load(){"
"if(loading){again=true;return;}"
"if(busyEditing()){later(1600);return;}"
"clearTimeout(retryT);retryT=0;"
"loading=true;again=false;"
// A failed or partial reply (the bridge sheds requests when memory is
// short) keeps the last good data instead of blanking the page; a device
// marked stale keeps its previous values.
"try{const r=await fetch('/control/data');"
// 503 = the bridge is busy with another request: try again shortly
// instead of waiting for the next 8 s tick.
"if(r.status===503){later(2000);throw 0;}"
"if(!r.ok)throw 0;const N=await r.json();"
"if(S&&S.devices&&N.devices)N.devices=N.devices.map(d=>d.stale?(S.devices.find(o=>o.mac===d.mac)||d):d);"
"if(N.ver!==undefined)VER=N.ver;"
"S=N;render();}"
"catch(e){}"
"finally{loading=false;if(again)later(200);}}"
// Live updates: the bridge counts every real change (from the controller,
// the app, Home Assistant or this page). The page asks for that counter
// every 1.5 s -- a few bytes -- and loads the full data only when it
// moved. A full load still happens every 60 s as a safety net, which also
// refreshes the controller's clock and uptime.
"let lastFull=0;"
"async function checkVer(){"
"if(document.hidden)return;"
"if(loading||retryT)return;"
"if(Date.now()-lastFull>60000){lastFull=Date.now();load();return;}"
"try{const r=await fetch('/control/ver',{cache:'no-store'});if(!r.ok)return;"
"const j=await r.json();if(j.v!==VER){lastFull=Date.now();load();}}catch(e){}}"
// A hidden tab does not poll: two forgotten tabs used to keep the bridge
// busy around the clock. On return it loads at once.
"lastFull=Date.now();load();setInterval(checkVer,1500);"
"document.addEventListener('visibilitychange',()=>{if(!document.hidden){lastFull=Date.now();load();}});"
// Ticks the cleaning countdown every second between reloads; each reload
// resets it to the controller's own value.
// Anchored to the wall clock at render time rather than decremented, so it
// stays exact when the browser throttles a background tab.
"setInterval(()=>{const e=document.getElementById('cleanleft');if(!e)return;"
"if(!e.dataset.t0)e.dataset.t0=Date.now();"
"const s=Math.max(0,(parseInt(e.dataset.s)||0)-Math.floor((Date.now()-e.dataset.t0)/1000));"
"e.textContent=Math.floor(s/3600)+':'+String(Math.floor(s/60)%60).padStart(2,'0')"
"+':'+String(s%60).padStart(2,'0');},250);"
"</script></body></html>";

esp_err_t control_page_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, CONTROL_PAGE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NAV_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, CONTROL_BODY_OPEN, HTTPD_RESP_USE_STRLEN);
    nav_send(req, NAV_CONTROL);
    httpd_resp_send_chunk(req, CONTROL_PAGE_BODY, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ---------------------------------------------------------------------------
// JSON feed: current values straight from the device cache
// ---------------------------------------------------------------------------

static int blk_int(cJSON *b, const char *a, const char *bb, int def)
{
    if (!b) return def;
    cJSON *v = cJSON_GetObjectItem(b, a);
    if (!cJSON_IsNumber(v) && bb) v = cJSON_GetObjectItem(b, bb);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static void period_time(cJSON *b, const char *arr_key, const char *field,
                        char *out, size_t out_sz)
{
    strncpy(out, "00:00", out_sz - 1);
    out[out_sz - 1] = '\0';
    if (!b) return;
    cJSON *arr = cJSON_GetObjectItem(b, arr_key);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) return;
    cJSON *p = cJSON_GetArrayItem(arr, 0);
    cJSON *t = cJSON_GetObjectItem(p, field);
    if (cJSON_IsNumber(t)) sf_seconds_to_hhmm((int)t->valuedouble, out, out_sz);
}

static int period_int(cJSON *b, const char *arr_key, const char *field, int def)
{
    if (!b) return def;
    cJSON *arr = cJSON_GetObjectItem(b, arr_key);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) return def;
    cJSON *p = cJSON_GetArrayItem(arr, 0);
    cJSON *t = cJSON_GetObjectItem(p, field);
    return cJSON_IsNumber(t) ? (int)t->valuedouble : def;
}

static const char *fan_mode_name(int mt)
{
    switch (mt) {
        case 0:  return "Manual";
        case 1:  return "Schedule";
        case 2:  return "Cycle";
        case 3:  return "Environment: Temperature only";
        case 4:  return "Environment: Humidity only";
        case 7:  return "Environment: Prioritize temperature";
        case 8:  return "Environment: Prioritize humidity";
        case 13: return "Environment: Temperature & humidity";
        default: return "Manual";
    }
}

static const char *light_mode_name(int mt)
{
    switch (mt) {
        case 1:  return "Schedule";
        case 12: return "PPFD";
        default: return "Manual";
    }
}

// Slug of the device being built (set by build_device), so add_fan needs
// no registry lock while the cache lock is held.
static const char *s_build_slug = NULL;

static void add_fan(cJSON *root, const char *mac, const char *key)
{
    cJSON *b = device_cache_get(mac, key);
    if (!b) return;

    // Speeds and oscillation are published to Home Assistant in percent,
    // in steps of ten, the way the app shows them. The fan's wire values
    // are 1-10 and its shake 0-10, so they are scaled here rather than
    // in two places -- otherwise the page would offer the blower's
    // percentages and the fan's wire numbers under the same label.
    bool circ = (strcmp(key, "fan") == 0);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "on", blk_int(b, "on", "mOnOff", 0) != 0);
    // Held at the module's floor while stopped, so the control shows a
    // speed it can be switched back on at rather than a 0 that is not in
    // its own option list.
    int lvl = blk_int(b, "level", "mLevel", 0);
    if (circ && lvl < 1) lvl = 1;
    // In the module's own units, as the app shows them (fan 1-10,
    // oscillation 0-10, blower 25-100).
    cJSON_AddNumberToObject(o, "level", lvl);
    cJSON_AddNumberToObject(o, "shake", blk_int(b, "shakeLevel", NULL, 0));
    if (circ && s_build_slug) cJSON_AddNumberToObject(o, "shake_last", sf_osc_last(s_build_slug));
    cJSON_AddBoolToObject(o, "natural", blk_int(b, "natural", NULL, 0) != 0);

    // Exhaust blower only. Left absent rather than defaulted when the
    // controller has not reported it, so the page can show "--" instead
    // of asserting a state it does not know.
    cJSON *co2 = cJSON_GetObjectItem(b, "closeCO2");
    if (cJSON_IsNumber(co2)) {
        cJSON_AddBoolToObject(o, "co2", co2->valuedouble != 0);
    }

    // Schedule speed for the Environment modes: Auto (the controller's 0)
    // or a step 1-10 on both modules. Sent as a string so it matches the
    // page's option list directly.
    int mx = blk_int(b, "maxSpeed", NULL, 0);
    if (mx <= 0) {
        cJSON_AddStringToObject(o, "maxSpeed", "Auto");
    } else {
        if (circ && mx > 10) mx = 10;
        if (!circ && mx < 25) mx = 25;
        if (!circ && mx > 100) mx = 100;
        char ms[12];
        snprintf(ms, sizeof(ms), "%d", mx);
        cJSON_AddStringToObject(o, "maxSpeed", ms);
    }
    // Standby is 0 for the circulation fan by definition, and Off for the
    // blower when the controller holds 0, so both are the word.
    int mn = blk_int(b, "minSpeed", NULL, 0);
    if (mn == 0) cJSON_AddStringToObject(o, "minSpeed", "Off");
    else          cJSON_AddNumberToObject(o, "minSpeed", mn);
    cJSON_AddStringToObject(o, "mode_label",
                            fan_mode_name(blk_int(b, "modeType", NULL, 0)));

    char t[8];
    period_time(b, "timePeriod", "startTime", t, sizeof(t));
    cJSON_AddStringToObject(o, "sched_start", t);
    period_time(b, "timePeriod", "endTime", t, sizeof(t));
    cJSON_AddStringToObject(o, "sched_end", t);

    cJSON *ct = cJSON_GetObjectItem(b, "cycleTime");
    char cs[8] = "00:00";
    char run_t[12] = "00:00:00", off_t[12] = "00:00:00";
    int run = 0, off = 0, times = 1;
    if (cJSON_IsObject(ct)) {
        cJSON *v = cJSON_GetObjectItem(ct, "startTime");
        if (cJSON_IsNumber(v)) sf_seconds_to_hhmm((int)v->valuedouble, cs, sizeof(cs));
        run   = blk_int(ct, "openDur", NULL, 0) / 60;
        off   = blk_int(ct, "closeDur", NULL, 0) / 60;
        times = blk_int(ct, "times", NULL, 1);
        // To the second, as the app shows them. The minute values above
        // stay for the existing fields.
        sf_seconds_to_hhmmss(blk_int(ct, "openDur", NULL, 0), run_t, sizeof(run_t));
        sf_seconds_to_hhmmss(blk_int(ct, "closeDur", NULL, 0), off_t, sizeof(off_t));
    }
    cJSON_AddStringToObject(o, "cyc_start", cs);
    cJSON_AddNumberToObject(o, "cyc_run", run);
    cJSON_AddNumberToObject(o, "cyc_off", off);
    cJSON_AddStringToObject(o, "cyc_run_t", run_t);
    cJSON_AddStringToObject(o, "cyc_off_t", off_t);
    cJSON_AddNumberToObject(o, "cyc_times", times);

    cJSON_AddItemToObject(root, key, o);
}

static void add_light(cJSON *root, const char *mac, const char *key)
{
    cJSON *b = device_cache_get(mac, key);
    if (!b) return;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "on", blk_int(b, "on", "mOnOff", 0) != 0);
    cJSON_AddNumberToObject(o, "level", blk_int(b, "level", "mLevel", 0));
    cJSON_AddStringToObject(o, "mode_label",
                            light_mode_name(blk_int(b, "modeType", NULL, 0)));
    cJSON_AddNumberToObject(o, "dark", blk_int(b, "darkTemp", NULL, 0));
    cJSON_AddNumberToObject(o, "offT", blk_int(b, "offTemp", NULL, 0));
    cJSON_AddNumberToObject(o, "ppfd_min", blk_int(b, "ppfdMinBrightness", NULL, 0));
    cJSON_AddNumberToObject(o, "ppfd_max", blk_int(b, "ppfdMaxBrightness", NULL, 100));

    char t[8];
    period_time(b, "timePeriod", "startTime", t, sizeof(t));
    cJSON_AddStringToObject(o, "sched_start", t);
    period_time(b, "timePeriod", "endTime", t, sizeof(t));
    cJSON_AddStringToObject(o, "sched_end", t);
    cJSON_AddNumberToObject(o, "sched_bri", period_int(b, "timePeriod", "brightness", 0));
    cJSON_AddNumberToObject(o, "fade", period_int(b, "timePeriod", "fadeTime", 0) / 60);

    period_time(b, "ppfdPeriod", "startTime", t, sizeof(t));
    cJSON_AddStringToObject(o, "ppfd_start", t);
    period_time(b, "ppfdPeriod", "endTime", t, sizeof(t));
    cJSON_AddStringToObject(o, "ppfd_end", t);
    cJSON_AddNumberToObject(o, "ppfd", period_int(b, "ppfdPeriod", "brightness", 0));
    cJSON_AddNumberToObject(o, "ppfd_fade", period_int(b, "ppfdPeriod", "fadeTime", 0) / 60);

    cJSON_AddItemToObject(root, key, o);
}

// Builds the full block for one controller: its name, whether it is
// connected, and every cached value. One of these per tab.
static cJSON *build_device(const char *mac, const char *slug, const char *name)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddStringToObject(root, "slug", slug);
    cJSON_AddStringToObject(root, "name", name);
    // What an empty field falls back to, shown read-only on the page.
    char dn[DEV_NAME_LEN], ds[DEV_SLUG_LEN];
    device_registry_default_name(mac, dn, sizeof(dn));
    device_registry_default_slug(mac, ds, sizeof(ds));
    cJSON_AddStringToObject(root, "def_name", dn);
    cJSON_AddStringToObject(root, "def_slug", ds);
    cJSON_AddBoolToObject(root, "online", mitm_proxy_device_online(mac));

    device_cache_lock();

    s_build_slug = slug;
    add_fan(root, mac, "fan");
    add_fan(root, mac, "blower");
    s_build_slug = NULL;
    add_light(root, mac, "light");
    add_light(root, mac, "light2");

    // Climate accessories
    cJSON *climate = cJSON_CreateArray();
    const struct { const char *key; const char *name; } acc[] = {
        { "heater",       "Heater"       },
        { "humidifier",   "Humidifier"   },
        { "dehumidifier", "Dehumidifier" },
    };
    for (int i = 0; i < 3; i++) {
        cJSON *b = device_cache_get(mac, acc[i].key);
        if (!b) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key", acc[i].key);
        cJSON_AddStringToObject(o, "name", acc[i].name);
        cJSON_AddBoolToObject(o, "on", blk_int(b, "mOnOff", "on", 0) != 0);
        cJSON_AddItemToArray(climate, o);
    }
    cJSON_AddItemToObject(root, "climate", climate);

    device_cache_unlock();

    // Day cycle targets.
    //
    // Read from the cache like the module blocks above. Not a device
    // module, so it carries no on/off or level of its own -- just the
    // window that separates day from night and the three setpoints.
    device_cache_lock();
    cJSON *tb = device_cache_get(mac, "target");
    if (cJSON_IsObject(tb)) {
        cJSON_AddItemToObject(root, "target", cJSON_Duplicate(tb, true));
    }
    device_cache_unlock();
    // Grow plan: sent separately as raw text (see control_data_handler),
    // so the device tree stays small. Here only whether there is one, and
    // whether the bridge drives it from a long plan of its own.
    cJSON_AddBoolToObject(root, "has_plan", device_cache_plan_known(mac));
    {
        plan_store_info_t li;
        cJSON_AddBoolToObject(root, "lplan_active", SB_LONG_PLAN && plan_store_info(mac, &li) && li.active);
    }
    {
        // Today as the plan encodes dates, for "day N of the stage".
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        if (t.tm_year + 1900 >= 2020)
            cJSON_AddNumberToObject(root, "today",
                ((t.tm_year + 1900) << 16) | ((t.tm_mon + 1) << 8) | t.tm_mday);
    }

    // Sensor calibration.
    //
    // Read from the registry rather than the controller: the controller
    // applies these offsets but never reports them back, so this side
    // holds the only record of what was set.
    float ct, ch, cc, cp;
    device_registry_get_cal(mac, &ct, &ch, &cc, &cp);
    cJSON *cal = cJSON_CreateObject();
    cJSON_AddNumberToObject(cal, "temp", ct);
    cJSON_AddNumberToObject(cal, "humi", ch);
    cJSON_AddNumberToObject(cal, "co2", cc);
    cJSON_AddNumberToObject(cal, "ppfd", cp);
    cJSON_AddItemToObject(root, "cal", cal);

    // Sensor cleaning. Reported as null while it has never been decided,
    // so the interface can say so rather than showing an off switch that
    // might merely be out of date.
    {
        bool known = false;
        bool on = device_cache_sensor_cleaning(mac, &known, NULL);
        int phase = 0, remain = 0;
        bool reported = device_cache_sensor_cleaning_phase(mac, &phase, &remain);
        if (reported) on = (phase == 1);
        cJSON *clean = cJSON_CreateObject();
        // on: the switch (cleaning running). phase: 0 idle, 1 cleaning,
        // 2 cooldown. remain: seconds left in the current phase, as the
        // controller reports it (counted down between its frames).
        cJSON_AddBoolToObject(clean, "on", on);
        cJSON_AddBoolToObject(clean, "known", known || reported);
        cJSON_AddNumberToObject(clean, "phase", reported ? phase : (on ? 1 : 0));
        cJSON_AddNumberToObject(clean, "remain", reported ? remain : 0);
        cJSON_AddItemToObject(root, "clean", clean);
    }

    // Alarm settings, as the controller holds them, plus the field
    // description (labels, ranges, steps) so the page needs no copy of it.
    {
        device_cache_lock();
        cJSON *ab = device_cache_get(mac, "alarm");
        if (cJSON_IsObject(ab)) {
            cJSON_AddItemToObject(root, "alarm", cJSON_Duplicate(ab, true));
        }
        device_cache_unlock();
        cJSON *spec = cJSON_CreateObject();
        cJSON *rs = cJSON_CreateArray();
        for (int i = 0; i < ALARM_RANGE_COUNT; i++) {
            const alarm_range_t *r = &ALARM_RANGES[i];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "k", r->key);
            cJSON_AddStringToObject(o, "l", r->label);
            cJSON_AddStringToObject(o, "u", r->unit);
            cJSON_AddBoolToObject(o, "mn", r->has_min);
            cJSON_AddNumberToObject(o, "nlo", r->min_lo);
            cJSON_AddNumberToObject(o, "nhi", r->min_hi);
            cJSON_AddNumberToObject(o, "xlo", r->max_lo);
            cJSON_AddNumberToObject(o, "xhi", r->max_hi);
            cJSON_AddNumberToObject(o, "st", r->step);
            cJSON_AddItemToArray(rs, o);
        }
        cJSON_AddItemToObject(spec, "r", rs);
        cJSON *ss = cJSON_CreateArray();
        for (int i = 0; i < ALARM_SWITCH_COUNT; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "k", ALARM_SWITCHES[i].key);
            cJSON_AddStringToObject(o, "l", ALARM_SWITCHES[i].label);
            cJSON_AddItemToArray(ss, o);
        }
        cJSON_AddItemToObject(spec, "s", ss);
        cJSON_AddItemToObject(root, "alarm_spec", spec);
    }

    // The zone the controller itself reports in getSysSta.
    device_registry_lock();
    device_entry_t *d = device_registry_find(mac);
    if (d && d->tz_known) {
        cJSON_AddStringToObject(root, "tz_name", d->tz_name);
        cJSON_AddStringToObject(root, "tz_posix", d->tz_posix);
    }
    device_registry_unlock();

    // What the controller reports about itself.
    //
    // Taken from the cached status frame rather than kept in a second
    // place, so the page cannot disagree with the MQTT topics.
    device_cache_lock();
    cJSON *sysb = device_cache_get(mac, "sys");
    if (cJSON_IsObject(sysb)) {
        cJSON_AddItemToObject(root, "sys", cJSON_Duplicate(sysb, true));
    }
    device_cache_unlock();

    // Sensors and outlets are not module blocks; they come from the
    // latest status frame of this particular controller.
    cJSON *sensors = cJSON_CreateObject();
    cJSON *outlets = cJSON_CreateArray();
    mitm_proxy_fill_live_state(mac, sensors, outlets);
    cJSON_AddItemToObject(root, "sensors", sensors);
    cJSON_AddItemToObject(root, "outlets", outlets);

    return root;
}

// ---------------------------------------------------------------------------
// GET /control/zones
//
// The zone names, served from the firmware's own table so the page and
// the command handler cannot disagree about which zones exist.
// ---------------------------------------------------------------------------
esp_err_t control_zones_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);

    char item[64];
    for (int i = 0; i < TZ_TABLE_COUNT; i++) {
        int n = snprintf(item, sizeof(item), "%s\"%s\"",
                         i ? "," : "", TZ_TABLE[i].name);
        httpd_resp_send_chunk(req, item, n);
    }

    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t control_data_impl(httpd_req_t *req);

// GET /control/ver -> {"v":N}: the change counter, a few bytes. An open
// page asks this every couple of seconds and fetches the full feed only
// when it moved, so changes show up at once without the feed's cost.
esp_err_t control_ver_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    char b[32];
    int n = snprintf(b, sizeof(b), "{\"v\":%lu}", (unsigned long)device_cache_version());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, b, n);
}

esp_err_t control_data_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    if (!web_heavy_begin(req)) return ESP_OK;
    esp_err_t r = control_data_impl(req);
    web_heavy_end();
    return r;
}

static esp_err_t control_data_impl(httpd_req_t *req)
{

    // Snapshot the registry first, so the lock is not held while building
    // the much larger per-device payloads.
    char macs[SB_MAX_DEVICES][DEV_MAC_LEN];
    char slugs[SB_MAX_DEVICES][DEV_SLUG_LEN];
    char names[SB_MAX_DEVICES][DEV_NAME_LEN];
    int count = 0;

    device_registry_lock();
    for (int i = 0; i < SB_MAX_DEVICES; i++) {
        device_entry_t *d = device_registry_at(i);
        if (!d) continue;
        strncpy(macs[count],  d->mac,  DEV_MAC_LEN - 1);  macs[count][DEV_MAC_LEN - 1]   = '\0';
        strncpy(slugs[count], d->slug, DEV_SLUG_LEN - 1); slugs[count][DEV_SLUG_LEN - 1] = '\0';
        strncpy(names[count], d->name, DEV_NAME_LEN - 1); names[count][DEV_NAME_LEN - 1] = '\0';
        count++;
    }
    device_registry_unlock();

    // Streamed device by device: with the grow plan in it the whole feed
    // is several KB, and one contiguous print of all of it failed on a
    // fragmented heap ("Out of memory") right after a plan write. Each
    // device is printed and freed on its own, so the peak is one device.
    sb_prov_cfg_t *prov = malloc(sizeof(*prov));
    if (!prov) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    sb_prov_load(prov);
    cJSON *head = cJSON_CreateObject();
    // Taken before the feed is built: a change while building raises the
    // counter past this, so the page asks again rather than missing it.
    cJSON_AddNumberToObject(head, "ver", device_cache_version());
    cJSON_AddNumberToObject(head, "plan_max", SB_PLAN_MAX_STAGES);
    cJSON_AddBoolToObject(head, "session", mitm_proxy_has_session());
    // Whether the bridge is pushing its own clock settings. When it is,
    // the per-device zone control is shown as read-only -- otherwise the
    // page would offer a choice that is overwritten on every reconnect.
    cJSON_AddBoolToObject(head, "tz_push", prov->tz_push);
    cJSON_AddStringToObject(head, "bridge_tz", prov->tz_name);
    cJSON_AddStringToObject(head, "bridge_name", prov_bridge_name());
    {
        char def[33];
        prov_default_bridge_name(def, sizeof(def));
        cJSON_AddStringToObject(head, "bridge_def", def);
        cJSON_AddBoolToObject(head, "bridge_is_def", prov_bridge_name_is_default());
    }
    free(prov);
    char *hj = cJSON_PrintUnformatted(head);
    cJSON_Delete(head);
    if (!hj) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    // Replace the closing brace with the devices array.
    size_t hl = strlen(hj);
    if (hl && hj[hl - 1] == '}') hl--;
    esp_err_t err = httpd_resp_send_chunk(req, hj, hl);
    cJSON_free(hj);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, ",\"devices\":[", 12);
    for (int i = 0; i < count && err == ESP_OK; i++) {
        cJSON *d = build_device(macs[i], slugs[i], names[i]);
        char *dj = d ? cJSON_PrintUnformatted(d) : NULL;
        cJSON_Delete(d);
        if (!dj) {
            // Low heap for a moment (a TLS record in flight): still a valid
            // document, with the device marked so the page keeps the last
            // values instead of showing it as gone.
            ESP_LOGW(TAG, "control/data: no memory for %s (heap %u)", macs[i],
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            char stub[64];
            int sl = snprintf(stub, sizeof(stub), "%s{\"mac\":\"%s\",\"stale\":true}", i ? "," : "", macs[i]);
            err = httpd_resp_send_chunk(req, stub, sl);
            continue;
        }
        if (i) err = httpd_resp_send_chunk(req, ",", 1);
        // The device object without its closing brace, then the plan
        // spliced in as stored text (no parse, no second tree), then "}".
        size_t dl = strlen(dj);
        if (dl && dj[dl - 1] == '}') dl--;
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, dj, dl);
        cJSON_free(dj);
        if (err == ESP_OK) {
            char *pt = malloc(SB_TLS_RX_BUF_SIZE);
            int en = -1;
            size_t pl = pt ? device_cache_plan_text(macs[i], pt, SB_TLS_RX_BUF_SIZE, &en) : 0;
            if (pl > 2 && pt[pl - 1] == '}') {
                char tail[32];
                int tl = snprintf(tail, sizeof(tail), ",\"enabled\":%d}", en > 0 ? 1 : 0);
                err = httpd_resp_send_chunk(req, ",\"plan\":", 8);
                if (err == ESP_OK) err = httpd_resp_send_chunk(req, pt, pl - 1);
                if (err == ESP_OK) err = httpd_resp_send_chunk(req, tail, tl);
            }
            free(pt);
        }
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, "}", 1);
    }
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

// ---------------------------------------------------------------------------
// POST /control/set
//
// Uses the exact same translation path as an MQTT command, so the web
// interface and Home Assistant cannot drift apart.
// ---------------------------------------------------------------------------

static void url_decode_inplace(char *s)
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

static bool form_field(const char *body, const char *key, char *out, size_t out_sz)
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
            url_decode_inplace(out);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

// Reads a form body of at most max bytes into a new heap string.
static char *read_body(httpd_req_t *req, int max)
{
    if (req->content_len <= 0 || req->content_len > max) return NULL;
    char *body = malloc(req->content_len + 1);
    if (!body) return NULL;
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) { free(body); return NULL; }
        got += r;
    }
    body[got] = '\0';
    return body;
}

static void reply_text(httpd_req_t *req, bool ok, const char *msg)
{
    httpd_resp_set_type(req, "text/plain");
    if (!ok) httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_send(req, msg ? msg : (ok ? "ok" : "Failed"), HTTPD_RESP_USE_STRLEN);
}

// GET /control/templates -> {"system":[{id,tpl}],"custom":[{id,tpl|null}]}
static bool chunk_sink(void *ctx, const char *s, size_t n)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, s, n) == ESP_OK;
}

esp_err_t control_templates_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    if (!web_heavy_begin(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    plan_tpl_list(chunk_sink, req);
    httpd_resp_send_chunk(req, NULL, 0);
    web_heavy_end();
    return ESP_OK;
}

// POST /control/template  a=save&id=cust:N&name=..&v=<template json>
//                         a=delete&id=cust:N
//                         a=clone&src=<id>[&id=cust:N]&name=..
esp_err_t control_template_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    char *body = read_body(req, 4000);
    if (!body) { reply_text(req, false, "Bad request"); return ESP_OK; }
    size_t bl = strlen(body);
    char a[12] = "", id[16] = "", src[16] = "", name[48] = "";
    form_field(body, "a", a, sizeof(a));
    form_field(body, "id", id, sizeof(id));
    form_field(body, "src", src, sizeof(src));
    form_field(body, "name", name, sizeof(name));
    bool ok = false;
    const char *msg = "Unknown action";
    char newid[16] = "";
    if (strcmp(a, "save") == 0) {
        char *v = malloc(bl + 1);
        if (v) {
            form_field(body, "v", v, bl + 1);
            ok = plan_tpl_save(id, name, v);
            msg = ok ? "Template saved" : "Template not saved (invalid)";
            free(v);
        }
    } else if (strcmp(a, "delete") == 0) {
        ok = plan_tpl_delete(id);
        msg = ok ? "Template deleted" : "Not a custom template";
    } else if (strcmp(a, "clone") == 0) {
        ok = plan_tpl_clone(src, id[0] ? id : NULL, name, newid, sizeof(newid));
        msg = ok ? newid : "No free custom slot (5 at most) or unknown template";
    }
    free(body);
    // The Home Assistant template select lists the names: refresh just it.
    if (ok) ha_mqtt_refresh_plan_template_selects();
    reply_text(req, ok, msg);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Long plans on the bridge
//
// GET  /control/lplan?d=<mac>&from=<i>&n=<k>
//   -> {"active":..,"running":..,"count":N,"window":W,"max":27,"from":i,
//       "stage":[...k stages from i...]}   (streamed, one stage at a time)
// POST /control/lplan  d=<mac>&a=stage|delete|template|clear|activate|
//                       deactivate|start|stop|import&v=...
// ---------------------------------------------------------------------------
static int query_int(httpd_req_t *req, const char *key, int def)
{
    char q[96], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return def;
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return def;
    return atoi(v);
}

esp_err_t control_lplan_get_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    char q[96], mac[DEV_MAC_LEN] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "d", mac, sizeof(mac));
    if (!mac[0]) { const char *m = mitm_proxy_controller_mac(); if (m) strncpy(mac, m, sizeof(mac) - 1); }
    int from = query_int(req, "from", 0), k = query_int(req, "n", 10);
    if (from < 0) from = 0;
    if (k < 1 || k > 10) k = 10;
    if (!web_heavy_begin(req)) return ESP_OK;
    plan_store_info_t info = {0};
    bool have = plan_store_info(mac, &info);
    char *buf = malloc(PLAN_STORE_STAGE_MAX + 8);
    // The last stage's end, so a new stage can start the day after it
    // even when that stage is on another page.
    long last_end = 0;
    if (buf && have && info.count &&
        plan_store_get(mac, info.count - 1, buf, PLAN_STORE_STAGE_MAX + 8))
        last_end = jtext_int(buf, strlen(buf), "endDate", 0);
    char head[224];
    int hl = snprintf(head, sizeof(head),
        "{\"active\":%s,\"running\":%s,\"count\":%d,\"window\":%d,\"max\":%d,\"from\":%d,"
        "\"last_end\":%ld,\"stage\":[",
        info.active ? "true" : "false", info.running ? "true" : "false",
        have ? info.count : 0, have ? info.window : -1, PLAN_STORE_MAX_STAGES, from, last_end);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, head, hl);
    int sent = 0;
    for (int i = from; buf && have && i < info.count && sent < k; i++) {
        size_t l = plan_store_get(mac, i, buf, PLAN_STORE_STAGE_MAX + 8);
        if (!l) continue;
        if (sent++) httpd_resp_send_chunk(req, ",", 1);
        httpd_resp_send_chunk(req, buf, l);
    }
    free(buf);
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    web_heavy_end();
    return ESP_OK;
}

esp_err_t control_lplan_post_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    char *body = read_body(req, 4000);
    if (!body) { reply_text(req, false, "Bad request"); return ESP_OK; }
    size_t bl = strlen(body);
    char a[16] = "", mac[DEV_MAC_LEN] = "";
    form_field(body, "a", a, sizeof(a));
    form_field(body, "d", mac, sizeof(mac));
    char *v = malloc(bl + 1);
    if (v) { v[0] = '\0'; form_field(body, "v", v, bl + 1); }
    free(body);
    if (!mac[0]) { const char *m = mitm_proxy_controller_mac(); if (m) strncpy(mac, m, sizeof(mac) - 1); }
    char msg[128] = "Unknown action";
    bool ok = false, repush = false;
    if (!v || !mac[0]) {
        snprintf(msg, sizeof(msg), "No controller");
    } else if (!SB_LONG_PLAN && strcmp(a, "clear") != 0 && strcmp(a, "deactivate") != 0) {
        // The plan lives on the controller only (SB_LONG_PLAN 0). Only
        // housekeeping on the stored copy is allowed; nothing here may
        // write the controller's plan.
        snprintf(msg, sizeof(msg), "Disabled in this firmware: the plan is kept on the controller");
    } else if (strcmp(a, "stage") == 0) {
        ok = plan_store_put(mac, v, msg, sizeof(msg)) >= 0;
        if (ok) { snprintf(msg, sizeof(msg), "Stage saved"); repush = true; }
    } else if (strcmp(a, "template") == 0) {
        // Append a stage from a template, after the last stage (or today).
        plan_store_info_t info = {0};
        plan_store_info(mac, &info);
        int start = sf_plan_today_packed();
        if (info.count) {
            char *last = malloc(PLAN_STORE_STAGE_MAX + 8);
            if (last && plan_store_get(mac, info.count - 1, last, PLAN_STORE_STAGE_MAX + 8)) {
                long e = jtext_int(last, strlen(last), "endDate", 0);
                if (e) start = sf_plan_date_add_days((int)e, 1);
            }
            free(last);
        }
        char *id = v;
        int days = 0;
        char *at = strchr(v, '@');
        if (at) { days = atoi(at + 1); *at = '\0'; }
        char *stage = start ? plan_tpl_make_stage(id, start, days) : NULL;
        if (!stage) snprintf(msg, sizeof(msg), start ? "Unknown template" : "Clock not set yet");
        else {
            ok = plan_store_put(mac, stage, msg, sizeof(msg)) >= 0;
            if (ok) { snprintf(msg, sizeof(msg), "Stage added"); repush = true; }
            free(stage);
        }
    } else if (strcmp(a, "delete") == 0) {
        ok = plan_store_delete(mac, atol(v));
        snprintf(msg, sizeof(msg), ok ? "Stage deleted" : "No such stage");
        repush = ok;
    } else if (strcmp(a, "clear") == 0) {
        ok = plan_store_clear(mac);
        snprintf(msg, sizeof(msg), ok ? "Long plan deleted" : "No long plan");
    } else if (strcmp(a, "import") == 0) {
        // Take over the controller's current plan as the start of a long one.
        char *pt = malloc(SB_TLS_RX_BUF_SIZE);
        int en = -1;
        size_t pl = pt ? device_cache_plan_text(mac, pt, SB_TLS_RX_BUF_SIZE, &en) : 0;
        int n = pl ? plan_store_import(mac, pt, pl) : 0;
        free(pt);
        ok = pl > 0;
        snprintf(msg, sizeof(msg), ok ? "%d stage(s) taken over from the controller" : "The controller has not reported a plan", n);
    } else if (strcmp(a, "activate") == 0 || strcmp(a, "deactivate") == 0) {
        bool on = strcmp(a, "activate") == 0;
        ok = plan_store_set_active(mac, on);
        if (on) { plan_store_set_window(mac, -1); repush = true; }
        snprintf(msg, sizeof(msg), on ? "The bridge now drives this controller's plan"
                                      : "The controller's own plan is used again");
    } else if (strcmp(a, "start") == 0 || strcmp(a, "stop") == 0) {
        bool on = strcmp(a, "start") == 0;
        ok = plan_store_set_running(mac, on);
        char *cmd = sf_plan_command_text("plan", "enabled", on ? "ON" : "OFF", mac,
                                         mitm_proxy_uid_for(mac), msg, sizeof(msg));
        if (cmd) { mitm_proxy_inject_command_to(mac, cmd, strlen(cmd)); free(cmd); }
        snprintf(msg, sizeof(msg), on ? "Plan started" : "Plan stopped");
    }
    free(v);
    // Changes to an active long plan reach the controller at once.
    if (SB_LONG_PLAN && ok && repush) {
        plan_store_info_t info;
        if (plan_store_info(mac, &info) && info.active && mitm_proxy_device_online(mac)) {
            int today = sf_plan_today_packed();
            int cur = today ? plan_store_current(mac, today) : 0;
            if (cur >= 0) sf_plan_window_push(mac, mitm_proxy_uid_for(mac), cur);
        }
    }
    reply_text(req, ok, msg);
    return ESP_OK;
}

// POST /control/plan  d=<mac>&a=enable|disable|stage|delete|template|clear&v=...
//
// Per stage: the page sends one stage (~1 KB) at a time, never the plan.
esp_err_t control_plan_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;
    if (req->content_len <= 0 || req->content_len > 4000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }
    char *body = malloc(req->content_len + 1);
    char *value = malloc(req->content_len + 1);
    if (!body || !value) {
        free(body); free(value);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) { free(body); free(value); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed"); return ESP_FAIL; }
        received += r;
    }
    body[received] = '\0';

    char action[16] = "", mac_param[DEV_MAC_LEN] = "";
    form_field(body, "a", action, sizeof(action));
    form_field(body, "d", mac_param, sizeof(mac_param));
    form_field(body, "v", value, req->content_len + 1);
    const char *mac = mac_param[0] ? mac_param : mitm_proxy_controller_mac();
    char msg[128] = "Failed";
    bool ok = false;
    if (!mac || !mac[0] || !mitm_proxy_device_online(mac)) {
        snprintf(msg, sizeof(msg), "That controller is not connected");
    } else {
        const char *sub = NULL, *val = value;
        if (strcmp(action, "enable") == 0)        { sub = "enabled"; val = "ON"; }
        else if (strcmp(action, "disable") == 0)  { sub = "enabled"; val = "OFF"; }
        else if (strcmp(action, "stage") == 0)    { sub = "stage"; }
        else if (strcmp(action, "delete") == 0)   { sub = "delete"; }
        else if (strcmp(action, "template") == 0) { sub = "template"; }
        else if (strcmp(action, "clear") == 0)    { sub = "clear"; }
        if (!sub) {
            snprintf(msg, sizeof(msg), "Unknown action");
        } else {
            char *cmd = sf_plan_command_text("plan", sub, val, mac, mitm_proxy_uid_for(mac),
                                             msg, sizeof(msg));
            if (cmd) {
                ok = mitm_proxy_inject_command_to(mac, cmd, strlen(cmd));
                snprintf(msg, sizeof(msg), ok ? "Sent" : "Could not send to the controller");
                ESP_LOGI(TAG, "Web control: plan %s (%u bytes)", action, (unsigned)strlen(cmd));
                free(cmd);
                // Home Assistant sees the new state now; the controller's
                // own reply confirms it shortly.
                device_registry_lock();
                device_entry_t *de = device_registry_find(mac);
                char slug[DEV_SLUG_LEN] = "";
                if (de) strncpy(slug, de->slug, sizeof(slug) - 1);
                device_registry_unlock();
                if (slug[0]) sf_publish_plan(mac, slug);
            }
        }
    }
    free(body); free(value);
    reply_text(req, ok, msg);
    return ESP_OK;
}

esp_err_t control_set_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request size");
        return ESP_FAIL;
    }

    char body[520];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    char field[32] = "", subfield[32] = "", value[160] = "";
    char mac_param[DEV_MAC_LEN] = "";
    form_field(body, "f", field, sizeof(field));
    form_field(body, "s", subfield, sizeof(subfield));
    form_field(body, "v", value, sizeof(value));
    form_field(body, "d", mac_param, sizeof(mac_param));

    if (!field[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing field");
        return ESP_FAIL;
    }

    // The tab sends which device it belongs to. Falling back to the first
    // active session keeps older links working.
    const char *mac = mac_param[0] ? mac_param : mitm_proxy_controller_mac();
    if (!mac || !mac[0]) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "No controller connected", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!mitm_proxy_device_online(mac)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "That controller is not connected",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // The light entity takes a JSON payload rather than a bare value, so
    // brightness and mode changes go through the same shape Home
    // Assistant uses.
    char json_value[192];
    const char *effective = value;
    const char *effective_sub = subfield[0] ? subfield : NULL;

    // The zone picker sends a name; the command needs "Name|POSIX".
    //
    // Accepted even while the bridge manages the clock: the change goes
    // through and the sync puts it back, which is clearer than a refusal.
    if (strcmp(field, "timezone") == 0) {
        const char *posix = tz_posix_for(value);
        if (!posix) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, "Unknown time zone", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        snprintf(json_value, sizeof(json_value), "%s|%s", value, posix);
        effective = json_value;
        effective_sub = NULL;
    }

    bool is_light = (strcmp(field, "light") == 0 || strcmp(field, "light2") == 0);
    if (is_light && effective_sub && strcmp(effective_sub, "brightness") == 0) {
        snprintf(json_value, sizeof(json_value),
                 "{\"state\":\"ON\",\"brightness\":%d}", atoi(value));
        effective = json_value;
        effective_sub = NULL;
    } else if (is_light && effective_sub && strcmp(effective_sub, "effect") == 0) {
        snprintf(json_value, sizeof(json_value),
                 "{\"state\":\"ON\",\"effect\":\"%s\"}", value);
        effective = json_value;
        effective_sub = NULL;
    }

    if (strcmp(field, "time_sync") == 0) {
        bool ok = sf_sync_device_time(mac, mitm_proxy_uid_for(mac));
        httpd_resp_set_type(req, "text/plain");
        if (!ok) httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, ok ? "Device time synced" : "Not sent (bridge clock not set yet?)");
        return ESP_OK;
    }

    cJSON *cmd = sf_translate_command(field, effective_sub, effective,
                                      mac, mitm_proxy_uid_for(mac));
    if (!cmd) {
        // The usual reasons, said plainly instead of "Could not translate".
        bool fan = strcmp(field, "fan") == 0 || strcmp(field, "blower") == 0;
        const char *why = "Not accepted";
        if (fan && effective_sub && strcmp(effective_sub, "percentage") == 0)
            why = "Speed can only be changed in Manual mode";
        else if (fan && effective_sub && strcmp(effective_sub, "schedule_speed") == 0 &&
                 strcasecmp(effective, "Auto") == 0)
            why = "Auto is only available in Environment mode";
        char note[160];
        sf_command_take_note(note, sizeof(note));
        if (note[0]) why = note;   // the translator's own reason (e.g. Cycle times)
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, why);
        return ESP_OK;
    }

    char *json = cJSON_PrintUnformatted(cmd);
    if (json) {
        mitm_proxy_inject_command_to(mac, json, strlen(json));

        // Same optimistic update as the MQTT path, so the page shows the
        // new value on its next refresh instead of the old one.
        cJSON *params = cJSON_GetObjectItem(cmd, "params");
        if (cJSON_IsObject(params)) {
            const char *mods[] = { "fan", "blower", "light", "light2" };
            for (int i = 0; i < 4; i++) {
                cJSON *blk = cJSON_GetObjectItem(params, mods[i]);
                if (!cJSON_IsObject(blk)) continue;
                if (cJSON_GetObjectItem(blk, "modeType")) device_cache_note_mode_command(mac, mods[i]);
                device_cache_merge(mac, mods[i], blk);
            }

            // Day cycle targets, the same way. The controller only echoes
            // the target block back when it is asked for it, so without
            // this the page would keep showing the value that was set
            // before the last change -- and the next change would send
            // the stale block back to the controller.
            cJSON *tgt = cJSON_GetObjectItem(params, "target");
            if (cJSON_IsObject(tgt)) device_cache_merge(mac, "target", tgt);

            // Alarm block: replaced (an alarm turned off is a missing
            // "enabled", which a merge would keep) and echoed to Home
            // Assistant.
            cJSON *alm = cJSON_GetObjectItem(params, "alarm");
            if (cJSON_IsObject(alm)) {
                device_cache_replace(mac, "alarm", alm);
                char slug_buf[DEV_SLUG_LEN] = "";
                device_registry_lock();
                device_entry_t *de = device_registry_find(mac);
                if (de) strncpy(slug_buf, de->slug, sizeof(slug_buf) - 1);
                device_registry_unlock();
                if (slug_buf[0]) sf_publish_alarm(mac, slug_buf);
            }

            // Calibration has to be remembered here: the controller
            // applies it but never reports it back, so this is the only
            // record of what the offsets are.
            cJSON *cal = cJSON_GetObjectItem(params, "calibration");
            if (cJSON_IsObject(cal)) {
                device_cache_merge(mac, "calibration", cal);
                const char *cf[] = { "temp", "humi", "co2", "ppfd" };
                for (int i = 0; i < 4; i++) {
                    cJSON *v = cJSON_GetObjectItem(cal, cf[i]);
                    if (cJSON_IsNumber(v)) {
                        device_registry_set_cal(mac, cf[i],
                                                (float)v->valuedouble);
                    }
                }

                // Tell the cloud too, as a synthetic reply rather than a
                // mirrored command -- see mitm_proxy_report_calibration().
                char *cal_json = cJSON_PrintUnformatted(cal);
                if (cal_json) {
                    mitm_proxy_report_calibration(mac, cal_json);
                    cJSON_free(cal_json);
                }
            }
        }
        // A cleaning cycle started from this page is also shown in Home
        // Assistant at once, rather than at the next status frame.
        if ((strcmp(field, "sensor_cleaning") == 0 || strcmp(field, "sensor_heating") == 0)) {
            char slug_buf[DEV_SLUG_LEN] = "";
            device_registry_lock();
            device_entry_t *de = device_registry_find(mac);
            if (de) strncpy(slug_buf, de->slug, sizeof(slug_buf) - 1);
            device_registry_unlock();
            if (slug_buf[0]) sf_publish_sensor_cleaning(mac, slug_buf);
        }

        cJSON_free(json);
        ESP_LOGI(TAG, "Web control: %s/%s = %s",
                 field, subfield[0] ? subfield : "-", value);
    }
    cJSON_Delete(cmd);

    // Something the command adjusted on its own is told to the user (e.g.
    // schedule speed Auto -> 1 on leaving Environment mode).
    char note[160];
    sf_command_take_note(note, sizeof(note));
    httpd_resp_set_type(req, "text/plain");
    if (note[0]) return httpd_resp_sendstr(req, note);
    return httpd_resp_send(req, "ok", 2);
}

// ---------------------------------------------------------------------------
// POST /control/device
//
// Renames or forgets a controller. Only the Home Assistant side changes:
// the controller keeps its MAC, and what the bridge exchanges with the
// vendor cloud is untouched by either operation.
// ---------------------------------------------------------------------------
esp_err_t control_device_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) return ESP_OK;

    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request size");
        return ESP_FAIL;
    }

    char body[520];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    char action[16] = "", mac[DEV_MAC_LEN] = "";
    char slug[DEV_SLUG_LEN] = "", name[DEV_NAME_LEN] = "";
    form_field(body, "a", action, sizeof(action));
    form_field(body, "d", mac, sizeof(mac));
    form_field(body, "slug", slug, sizeof(slug));
    form_field(body, "name", name, sizeof(name));

    httpd_resp_set_type(req, "text/plain");

    // The bridge's own Home Assistant name, which is not tied to a MAC.
    if (strcmp(action, "bridge") == 0) {
        ha_mqtt_rename_bridge(name);
        return httpd_resp_send(req, "ok", 2);
    }

    if (!mac[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing device");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");

    if (strcmp(action, "rename") == 0) {
        // Remember the previous slug: its retained topics have to be
        // cleared, or Home Assistant keeps the old entities alongside
        // the new ones.
        char old_slug[DEV_SLUG_LEN] = "";
        device_registry_lock();
        device_entry_t *d = device_registry_find(mac);
        if (d) {
            strncpy(old_slug, d->slug, sizeof(old_slug) - 1);
            old_slug[sizeof(old_slug) - 1] = '\0';
        }
        device_registry_unlock();

        if (!device_registry_rename(mac, slug, name)) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_send(req, "That name is already in use",
                                   HTTPD_RESP_USE_STRLEN);
        }

        if (strcmp(old_slug, slug) != 0) {
            ha_mqtt_rename_device(mac, old_slug);
        } else {
            ha_mqtt_rename_device(mac, NULL);
        }
        return httpd_resp_send(req, "ok", 2);
    }

    if (strcmp(action, "forget") == 0) {
        char gone_slug[DEV_SLUG_LEN] = "";
        device_registry_lock();
        device_entry_t *d = device_registry_find(mac);
        if (d) {
            strncpy(gone_slug, d->slug, sizeof(gone_slug) - 1);
            gone_slug[sizeof(gone_slug) - 1] = '\0';
        }
        device_registry_unlock();

        if (gone_slug[0]) ha_mqtt_forget_device(gone_slug);
        device_cache_clear(mac);
        device_registry_remove(mac);

        // Nothing blocks it from coming back: if that controller
        // publishes again it is simply learned anew.
        ESP_LOGI(TAG, "Forgot %s -- it reappears if it reconnects", mac);
        return httpd_resp_send(req, "ok", 2);
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
    return ESP_FAIL;
}
