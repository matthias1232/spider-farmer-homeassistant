/*
 * SpiderBridge installer overlay.
 *
 * ESP Web Tools opens its window (<ewt-install-dialog>) after "Connect". This
 * script adds SpiderBridge's own entries to that window and reuses its serial
 * connection (the dialog's Improv client), so nothing else has to open the port:
 *
 *   IP addresses                     what the bridge reports (needs firmware with CMD 0x40)
 *   Send Wi-Fi to device             join a home network
 *   Connect Wi-Fi & randomize ...    join a home network + new random hotspot password
 *   Randomize SpiderBridge ...       new random hotspot password only
 *
 * The extra commands (0x40..0x42) are SpiderBridge extensions to Improv Wi-Fi
 * Serial, see firmware/ggs/main/improv_serial.c. Pinned to esp-web-tools 10.4.0:
 * it uses the dialog's _client._sendRPCWithResponse(), which is not public API.
 */
(function () {
  'use strict';

  var SVG_NS = 'http://www.w3.org/2000/svg';
  var CMD = { NETINFO: 0x40, AP_PASS: 0x41, WIFI_AP: 0x42 };
  var TIMEOUT = { net: 8000, pass: 15000, wifi: 45000, scan: 20000 };

  var ICONS = {
    send: 'M2,21L23,12L2,3V10L17,12L2,14V21Z',
    dice: 'M5,3H19A2,2 0 0,1 21,5V19A2,2 0 0,1 19,21H5A2,2 0 0,1 3,19V5A2,2 0 0,1 5,3Z' +
          'M7.5,6.5A1.5,1.5 0 1,0 7.5,9.5A1.5,1.5 0 1,0 7.5,6.5Z' +
          'M16.5,14.5A1.5,1.5 0 1,0 16.5,17.5A1.5,1.5 0 1,0 16.5,14.5Z' +
          'M12,10.5A1.5,1.5 0 1,0 12,13.5A1.5,1.5 0 1,0 12,10.5Z',
    info: 'M12,2A10,10 0 1,0 12,22A10,10 0 1,0 12,2Z M11,10H13V17H11Z M11,7H13V9H11Z'
  };

  var ERRORS = {
    UNKNOWN_RPC_COMMAND: 'This firmware does not support this function. Install the latest ' +
      'SpiderBridge firmware first (Install SpiderBridge in the window behind this one).',
    UNABLE_TO_CONNECT: 'The device could not join that network. Check the name and password, ' +
      'and that it is a 2.4 GHz network. Nothing was changed on the device.',
    INVALID_RPC_PACKET: 'The device rejected the values (invalid or too long). Nothing was changed.',
    TIMEOUT: 'The device did not answer in time. Press the board\'s reset button, reconnect and try again.',
    UNKNOWN_ERROR: 'The device reported an unknown error.'
  };

  var CSS = [
    'dialog.sbo{border:1px solid #2c303a;border-radius:.7rem;background:#1a1d24;color:#e6e6e6;',
    '  padding:0;width:min(32rem,92vw);font:15px/1.45 system-ui,sans-serif;box-shadow:0 12px 40px #000a}',
    'dialog.sbo::backdrop{background:#000a}',
    '.sbo-head{padding:1rem 1.2rem .3rem}.sbo-head h2{margin:0;font-size:1.1rem;color:#8ab4f8}',
    '.sbo-body{padding:.4rem 1.2rem}.sbo-body p{margin:.5rem 0}',
    '.sbo-body label{display:block;font-size:.82rem;color:#9aa0aa;margin:.8rem 0 .25rem}',
    '.sbo-body select,.sbo-body input[type=text],.sbo-body input[type=password]{width:100%;font:inherit;',
    '  padding:.5rem .6rem;border-radius:.35rem;background:#0f1116;color:#e6e6e6;border:1px solid #3a3f4b}',
    '.sbo-body select:focus,.sbo-body input:focus{outline:2px solid #8ab4f8;outline-offset:1px}',
    '.sbo-body .chk{display:flex;gap:.5rem;align-items:center;margin:.5rem 0;font-size:.85rem;color:#9aa0aa}',
    '.sbo-actions{display:flex;justify-content:flex-end;gap:.6rem;padding:.8rem 1.2rem 1.1rem}',
    '.sbo-actions button{font:inherit;font-weight:600;border-radius:.35rem;padding:.55rem 1.1rem;cursor:pointer;',
    '  border:1px solid #8ab4f8;background:transparent;color:#8ab4f8}',
    '.sbo-actions button.primary{background:#8ab4f8;color:#13151a}',
    '.sbo-actions button:disabled{opacity:.5;cursor:default}',
    '.sbo-msg{border-radius:.4rem;padding:.55rem .75rem;margin:.6rem 0;font-size:.9rem}',
    '.sbo-msg.err{background:#3a1717;border:1px solid #8a2f2f;color:#f2a1a1}',
    '.sbo-msg.warn{background:#3a2a12;border:1px solid #8a6a2a;color:#f0c674}',
    '.sbo-msg.ok{background:#12301f;border:1px solid #2f7a4a;color:#9fe0b4}',
    '.sbo-secret{font:1.25rem/1.3 ui-monospace,Consolas,monospace;letter-spacing:.04em;word-break:break-all;',
    '  background:#0f1116;border:1px dashed #8ab4f8;border-radius:.4rem;padding:.7rem .8rem;margin:.4rem 0;user-select:all}',
    '.sbo-kv{font-size:.88rem;color:#aab0ba}.sbo-kv b{color:#e6e6e6}',
    '.sbo-spin{display:inline-block;width:1em;height:1em;border:2px solid #8ab4f8;border-right-color:transparent;',
    '  border-radius:50%;animation:sbo-r .8s linear infinite;vertical-align:-.15em;margin-right:.5rem}',
    '@keyframes sbo-r{to{transform:rotate(360deg)}}'
  ].join('\n');

  // ---- small helpers ------------------------------------------------------
  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  function icon(path) {
    var s = document.createElementNS(SVG_NS, 'svg');
    s.setAttribute('slot', 'start');
    s.setAttribute('viewBox', '0 0 24 24');
    var p = document.createElementNS(SVG_NS, 'path');
    p.setAttribute('d', path);
    p.setAttribute('fill', 'currentColor');
    p.setAttribute('fill-rule', 'evenodd');
    s.appendChild(p);
    return s;
  }

  function explain(e) {
    var k = typeof e === 'string' ? e : (e && e.message) || String(e);
    return ERRORS[k] || k;
  }

  // Improv strings: one length byte, then UTF-8.
  function enc(list) {
    var out = [];
    var te = new TextEncoder();
    list.forEach(function (s) {
      var b = te.encode(s);
      if (b.length > 255) throw new Error('INVALID_RPC_PACKET');
      out.push(b.length);
      for (var i = 0; i < b.length; i++) out.push(b[i]);
    });
    return out;
  }

  function rpc(client, cmd, args, timeout) {
    if (typeof client._sendRPCWithResponse !== 'function') {
      return Promise.reject(new Error('The installer component is a different version than this page expects.'));
    }
    return client._sendRPCWithResponse(cmd, args, timeout);
  }

  function headline(item) {
    var h = item.querySelector('[slot="headline"]');
    return h ? h.textContent.replace(/\s+/g, ' ').trim() : '';
  }

  function safeUrl(u) {
    return /^http:\/\/\d{1,3}(\.\d{1,3}){3}(:\d+)?\/?$/.test(u || '') ? u : '';
  }

  function makeItem(ic, title, support, onClick) {
    var li = document.createElement('ew-list-item');
    li.setAttribute('type', 'button');
    li.setAttribute('data-sbo', '1');
    li.appendChild(ic);
    var h = el('div', null, title);
    h.setAttribute('slot', 'headline');
    li.appendChild(h);
    var s = el('div');
    s.setAttribute('slot', 'supporting-text');
    if (support) s.textContent = support;
    li.appendChild(s);
    li.addEventListener('click', onClick);
    li.sbSupport = s;
    return li;
  }

  function setLines(node, lines) {
    node.textContent = '';
    lines.forEach(function (ln, i) {
      if (i) node.appendChild(document.createElement('br'));
      (Array.isArray(ln) ? ln : [ln]).forEach(function (part) {
        if (typeof part === 'string') node.appendChild(document.createTextNode(part));
        else node.appendChild(part);
      });
    });
  }

  function link(url) {
    var a = el('a', null, url);
    a.href = url; a.target = '_blank'; a.rel = 'noopener';
    a.style.color = '#8ab4f8';
    a.addEventListener('click', function (e) { e.stopPropagation(); });
    return a;
  }

  // ---- IP address entry ---------------------------------------------------
  // Strings of CMD 0x40: 0 uplink SSID, 1 uplink IP, 2 gateway, 3 hotspot SSID,
  // 4 hotspot IP, 5 hotspot MAC, 6 clients, 7 RSSI, 8 bridge name, 9 version.
  function renderNet(item, r) {
    var lines = [];
    var homeUrl = safeUrl(r[1] ? 'http://' + r[1] + '/' : '');
    if (r[1]) {
      lines.push([('Home network "' + r[0] + '": ' + r[1] + ' (gateway ' + r[2] + ', ' + r[7] + ' dBm)  ')]
        .concat(homeUrl ? [link(homeUrl)] : []));
    } else {
      lines.push('Home network: not connected — use "Send Wi-Fi to device"');
    }
    var apUrl = safeUrl(r[4] ? 'http://' + r[4] + '/' : '');
    lines.push([('Hotspot "' + r[3] + '": ' + r[4] + ', ' + r[6] + ' client(s)  ')]
      .concat(apUrl ? [link(apUrl)] : []));
    lines.push('Hotspot MAC ' + r[5] + ' · ' + r[8] + ' · firmware ' + r[9]);
    setLines(item.sbSupport, lines);
  }

  function loadNet(client, item, force) {
    if (!force && client.__sboNet) { renderNet(item, client.__sboNet); return; }
    setLines(item.sbSupport, ['Reading addresses from the device…']);
    rpc(client, CMD.NETINFO, [], TIMEOUT.net).then(function (r) {
      if (!r || r.length < 10) throw new Error('UNKNOWN_ERROR');
      client.__sboNet = r;
      renderNet(item, r);
    }).catch(function (e) {
      var k = typeof e === 'string' ? e : (e && e.message);
      setLines(item.sbSupport, [k === 'UNKNOWN_RPC_COMMAND'
        ? 'Not available on this firmware version. Install the latest SpiderBridge firmware to see the addresses here.'
        : 'Could not read the addresses (' + explain(e) + '). Click to retry.']);
    });
  }

  // ---- tool dialog (Send Wi-Fi / randomize) -------------------------------
  var TITLES = {
    wifi: 'Send Wi-Fi to device',
    'wifi-random': 'Connect Wi-Fi & randomize SpiderBridge Wi-Fi password',
    random: 'Randomize SpiderBridge Wi-Fi password'
  };

  function openTool(client, mode, onDone) {
    var d = document.createElement('dialog');
    d.className = 'sbo';
    var head = el('div', 'sbo-head'); var h2 = el('h2', null, TITLES[mode]); head.appendChild(h2);
    var body = el('div', 'sbo-body');
    var actions = el('div', 'sbo-actions');
    d.appendChild(head); d.appendChild(body); d.appendChild(actions);
    var busy = false;
    d.addEventListener('cancel', function (e) { if (busy) e.preventDefault(); });
    d.addEventListener('close', function () { d.remove(); });
    document.body.appendChild(d);

    function button(label, primary, fn) {
      var b = el('button', primary ? 'primary' : '', label);
      b.type = 'button'; b.addEventListener('click', fn); return b;
    }
    function setActions() {
      actions.textContent = '';
      Array.prototype.slice.call(arguments).forEach(function (b) { actions.appendChild(b); });
    }
    function progress(text) {
      busy = true;
      body.textContent = '';
      var p = el('p'); p.appendChild(el('span', 'sbo-spin')); p.appendChild(document.createTextNode(text));
      body.appendChild(p);
      setActions(button('Please wait…', false, function () {}));
      actions.firstChild.disabled = true;
    }
    function fail(msg, back) {
      busy = false;
      body.textContent = '';
      body.appendChild(el('div', 'sbo-msg err', msg));
      setActions(button('Close', false, function () { d.close(); }),
                 button('Back', true, back));
    }
    function copy(text, btn) {
      var done = function () { btn.textContent = 'Copied'; setTimeout(function () { btn.textContent = 'Copy password'; }, 1500); };
      if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(done, function () {});
    }
    function success(rows, secret, apSsid) {
      busy = false;
      body.textContent = '';
      body.appendChild(el('div', 'sbo-msg ok', 'Done. The bridge restarts to apply this (about 10 seconds).'));
      rows.forEach(function (r) { var p = el('p', 'sbo-kv'); p.appendChild(document.createTextNode(r[0] + ' ')); var b = el('b'); if (r[1].nodeType) b.appendChild(r[1]); else b.textContent = r[1]; p.appendChild(b); body.appendChild(p); });
      var copyBtn = null;
      if (secret) {
        body.appendChild(el('p', 'sbo-kv', 'New hotspot "' + apSsid + '" password — write it down, it is shown only now:'));
        body.appendChild(el('div', 'sbo-secret', secret));
        body.appendChild(el('div', 'sbo-msg warn', 'A controller connected to the hotspot loses its connection until it gets ' +
          'this password. Re-send it over Bluetooth from the bridge\'s web interface (Control → Bluetooth). ' +
          'You can also read it later on the bridge\'s settings page.'));
        copyBtn = button('Copy password', false, function () { copy(secret, copyBtn); });
      }
      var close = button('Close', true, function () { d.close(); });
      if (copyBtn) setActions(copyBtn, close); else setActions(close);
      onDone(true);
    }

    // -- random only
    function showRandomConfirm() {
      body.textContent = '';
      body.appendChild(el('p', null, 'Creates a new random 15-character password for the SpiderBridge hotspot — ' +
        'the Wi-Fi the Spider Farmer controller connects to.'));
      body.appendChild(el('div', 'sbo-msg warn', 'The bridge restarts. A controller on the hotspot is disconnected until it ' +
        'gets the new password (re-send it over Bluetooth from the bridge\'s web interface).'));
      setActions(button('Cancel', false, function () { d.close(); }),
                 button('Randomize password', true, runRandom));
    }
    function runRandom() {
      progress('Generating a new password on the device…');
      rpc(client, CMD.AP_PASS, enc(['']), TIMEOUT.pass).then(function (r) {
        success([], r[1], r[0] || 'SpiderBridge');
      }).catch(function (e) { fail(explain(e), showRandomConfirm); });
    }

    // -- Wi-Fi form
    function showForm(prev) {
      busy = false;
      body.textContent = '';
      var intro = mode === 'wifi-random'
        ? 'Joins your home Wi-Fi and creates a new random password for the SpiderBridge hotspot, in one step. ' +
          'Nothing is changed if the home network cannot be joined.'
        : 'Sends your home Wi-Fi name and password to the device over USB. The device joins it and remembers it.';
      body.appendChild(el('p', null, intro));

      var lblNet = el('label', null, 'Network'); lblNet.setAttribute('for', 'sbo-net');
      var sel = el('select'); sel.id = 'sbo-net';
      var scanning = document.createElement('option'); scanning.textContent = 'Scanning for networks…'; sel.appendChild(scanning);
      sel.disabled = true;
      var lblSsid = el('label', null, 'Network name'); lblSsid.setAttribute('for', 'sbo-ssid');
      var ssid = el('input'); ssid.type = 'text'; ssid.id = 'sbo-ssid'; ssid.maxLength = 32;
      ssid.autocomplete = 'off'; ssid.spellcheck = false;
      var lblPw = el('label', null, 'Password'); lblPw.setAttribute('for', 'sbo-pw');
      var pw = el('input'); pw.type = 'password'; pw.id = 'sbo-pw'; pw.maxLength = 64; pw.autocomplete = 'off';
      var chkRow = el('label', 'chk'); var chk = el('input'); chk.type = 'checkbox';
      chkRow.appendChild(chk); chkRow.appendChild(document.createTextNode('Show password'));
      chk.addEventListener('change', function () { pw.type = chk.checked ? 'text' : 'password'; });
      var msg = el('div', 'sbo-msg err'); msg.style.display = 'none';
      [lblNet, sel, lblSsid, ssid, lblPw, pw, chkRow, msg].forEach(function (n) { body.appendChild(n); });

      var nets = [];
      var OTHER = '__other__';
      function syncOther() { var other = sel.value === OTHER; lblSsid.style.display = ssid.style.display = other ? '' : 'none'; }
      client.scan(TIMEOUT.scan).then(function (list) { nets = list || []; }).catch(function () { nets = []; }).then(function () {
        sel.textContent = '';
        nets.forEach(function (n) {
          var o = document.createElement('option'); o.value = n.name;
          o.textContent = n.name + '  (' + n.rssi + ' dBm' + (n.secured ? '' : ', open') + ')';
          sel.appendChild(o);
        });
        var other = document.createElement('option'); other.value = OTHER; other.textContent = 'Other network…';
        sel.appendChild(other);
        if (prev && prev.ssid) { var known = nets.some(function (n) { return n.name === prev.ssid; }); sel.value = known ? prev.ssid : OTHER; ssid.value = known ? '' : prev.ssid; }
        else if (!nets.length) sel.value = OTHER;
        sel.disabled = false; syncOther();
      });
      sel.addEventListener('change', syncOther);
      if (prev && prev.pw) pw.value = prev.pw;
      syncOther();

      function submit() {
        var name = sel.value === OTHER ? ssid.value.trim() : sel.value;
        var pass = pw.value;
        var secured = sel.value === OTHER ? pass.length > 0 : (nets.filter(function (n) { return n.name === name; })[0] || {}).secured !== false;
        var te = new TextEncoder();
        var problem = '';
        if (!name) problem = 'Enter the network name.';
        else if (te.encode(name).length > 32) problem = 'The network name is longer than 32 bytes.';
        else if (secured && pass.length === 0) problem = 'Enter the Wi-Fi password.';
        else if (pass.length > 0 && (pass.length < 8 || te.encode(pass).length > 64)) problem = 'A Wi-Fi password has 8 to 64 characters.';
        if (problem) { msg.textContent = problem; msg.style.display = ''; return; }
        send(name, pass);
      }
      pw.addEventListener('keydown', function (e) { if (e.key === 'Enter') submit(); });
      setActions(button('Cancel', false, function () { d.close(); }),
                 button(mode === 'wifi-random' ? 'Connect & randomize' : 'Send to device', true, submit));
    }

    function send(name, pass) {
      var again = function () { showForm({ ssid: name, pw: pass }); };
      progress('Connecting the device to "' + name + '"… (up to 30 seconds)');
      if (mode === 'wifi') {
        client.provision(name, pass, TIMEOUT.wifi).then(function () {
          var url = safeUrl(client.nextUrl || '');
          var rows = [['Connected to', name]];
          if (url) rows.push(['Device address', link(url)]);
          success(rows);
        }).catch(function (e) { fail(explain(e), again); });
      } else {
        rpc(client, CMD.WIFI_AP, enc([name, pass, '']), TIMEOUT.wifi).then(function (r) {
          var url = safeUrl(r[0] || '');
          var rows = [['Connected to', name]];
          if (url) rows.push(['Device address', link(url)]);
          success(rows, r[2], r[1] || 'SpiderBridge');
        }).catch(function (e) { fail(explain(e), again); });
      }
    }

    if (mode === 'random') showRandomConfirm(); else showForm();
    d.showModal();
  }

  // ---- injection into the ESP Web Tools window ----------------------------
  function enhance(dialog, list) {
    if (list.querySelector('[data-sbo]')) return;
    var items = Array.prototype.filter.call(list.children, function (c) { return c.tagName === 'EW-LIST-ITEM'; });
    var logs = items.filter(function (i) { return headline(i) === 'Logs & Console'; })[0];
    if (!logs) return;                       // not the dashboard
    var client = dialog._client;

    if (client === null) {                   // no Improv answer: foreign or idle firmware
      list.insertBefore(makeItem(icon(ICONS.info), 'Wi-Fi, hotspot password & IP addresses',
        'Available once SpiderBridge firmware answers over USB. Right after flashing it does; later only for 5 minutes ' +
        'after power-up and only while no controller is known — press the board\'s reset button and reconnect.',
        function () {}), logs);
      return;
    }
    if (!client) return;                     // still connecting

    var net = makeItem(icon(ICONS.info), 'IP addresses & status', '', function () { loadNet(client, net, true); });
    var anchor = items[0] && items[0].nextSibling;
    list.insertBefore(net, anchor || logs);

    var wifiIcon = null;
    items.forEach(function (i) { if (/^(Connect to|Change) Wi-Fi$/.test(headline(i)) && i.querySelector('svg')) wifiIcon = i.querySelector('svg'); });
    function wifi() { return wifiIcon ? wifiIcon.cloneNode(true) : icon(ICONS.send); }
    var done = function (restarting) {
      if (!restarting) return;
      client.__sboNet = null;
      setLines(net.sbSupport, ['The bridge is restarting. Click here in about 10 seconds to read the addresses again.']);
    };
    list.insertBefore(makeItem(icon(ICONS.send), 'Send Wi-Fi to device',
      'Pick a home network and send its password over USB', function () { openTool(client, 'wifi', done); }), logs);
    list.insertBefore(makeItem(wifi(), 'Connect Wi-Fi & randomize SpiderBridge Wi-Fi password',
      'Join the home network and create a new random hotspot password', function () { openTool(client, 'wifi-random', done); }), logs);
    list.insertBefore(makeItem(icon(ICONS.dice), 'Randomize SpiderBridge Wi-Fi password',
      'New random password for the bridge\'s own hotspot', function () { openTool(client, 'random', done); }), logs);

    if (dialog._info && dialog._info.firmware && dialog._info.firmware !== 'SpiderBridge') {
      setLines(net.sbSupport, ['Only SpiderBridge firmware reports its addresses.']);
    } else {
      loadNet(client, net, false);
    }
  }

  var pending = new WeakMap();
  function schedule(dialog) {
    if (pending.get(dialog)) return;
    pending.set(dialog, true);
    requestAnimationFrame(function () {
      pending.set(dialog, false);
      var root = dialog.shadowRoot;
      if (!root) return;
      var lists = root.querySelectorAll('ew-list');
      for (var i = 0; i < lists.length; i++) enhance(dialog, lists[i]);
    });
  }

  function attach(dialog, tries) {
    if (dialog.__sbo) return;
    var root = dialog.shadowRoot;
    if (!root) {
      // The component creates its shadow root a moment after the element is added.
      if ((tries || 0) < 100) setTimeout(function () { attach(dialog, (tries || 0) + 1); }, 50);
      return;
    }
    dialog.__sbo = true;
    new MutationObserver(function () { schedule(dialog); }).observe(root, { childList: true, subtree: true });
    schedule(dialog);
  }

  function init() {
    var st = document.createElement('style');
    st.textContent = CSS;
    document.head.appendChild(st);
    document.querySelectorAll('ewt-install-dialog').forEach(attach);
    new MutationObserver(function (muts) {
      muts.forEach(function (m) {
        m.addedNodes.forEach(function (n) { if (n.tagName === 'EWT-INSTALL-DIALOG') attach(n); });
      });
    }).observe(document.body, { childList: true });
  }

  if (document.body) init(); else document.addEventListener('DOMContentLoaded', init);
})();
