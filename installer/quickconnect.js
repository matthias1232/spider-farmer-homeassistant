/*
 * SpiderBridge Quick Connect wizard.
 *
 * One dialog that does everything for a new board, in one go:
 *   1. connect the board (Chrome's serial port window)
 *   2. flash the newest firmware
 *   3. send the home Wi-Fi + a new random hotspot password
 *   4. optionally search for Spider Farmer GGS controllers on the first start and connect them
 *
 * Flashing uses esptool-js (the library ESP Web Tools is built on). Step 3 and 4 use
 * SpiderBridge's Improv extensions (firmware/ggs/main/improv_serial.c, commands 0x40..0x43),
 * through improv-wifi-serial-sdk-compatible packets written by this file itself so it does
 * not depend on any private API of the other installer window.
 *
 * Exposes window.SBQuickConnect = { open(opts), _test }.
 */
(function () {
  'use strict';

  var ESPTOOL_URL = 'https://cdn.jsdelivr.net/npm/esptool-js@0.6.0/+esm';
  var ESPTOOL_FALLBACK = 'https://unpkg.com/esptool-js@0.6.0/bundle.js';
  var SERIAL_BAUD = 115200;

  var CMD = { WIFI: 0x01, STATE: 0x02, INFO: 0x03, SCAN: 0x04, NETINFO: 0x40, WIFI_AP: 0x42, BLE: 0x43 };
  var TYPE = { STATE: 1, ERROR: 2, RPC: 3, RESULT: 4 };
  var IMPROV_ERR = { 1: 'INVALID_RPC_PACKET', 2: 'UNKNOWN_RPC_COMMAND', 3: 'UNABLE_TO_CONNECT', 254: 'TIMEOUT', 255: 'UNKNOWN_ERROR' };

  var TEXT = {
    UNABLE_TO_CONNECT: 'The board could not join that Wi-Fi network. Check the name and the password, and that it is a ' +
      '2.4 GHz network. Nothing else was changed on the board.',
    INVALID_RPC_PACKET: 'The board rejected the values.',
    UNKNOWN_RPC_COMMAND: 'The firmware on this board is older than this wizard: it cannot randomize the hotspot password ' +
      'or search for controllers. Install the current firmware first.',
    NOT_SPIDERBRIDGE: 'The board answers over USB, but it is not running SpiderBridge firmware. Install the firmware first.',
    TIMEOUT: 'The board did not answer in time.',
    UNKNOWN_ERROR: 'The board reported an unknown error.'
  };

  // Failures after which "install the firmware and continue" is the right next step.
  var OFFER_INSTALL = { UNKNOWN_RPC_COMMAND: 1, NOT_SPIDERBRIDGE: 1, FOREIGN_FIRMWARE: 1, LISTENER_OFF: 1 };

  // ---- generic helpers ----------------------------------------------------
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  function explain(e) {
    if (e && e.detail) return e.detail;
    var k = (e && e.improv) || (typeof e === 'string' ? e : e && e.message) || String(e);
    return TEXT[k] || (e && e.message) || k;
  }

  // What the board printed while we waited tells us why it did not answer over USB.
  // Returns { code, detail }.
  function diagnose(link) {
    var t = String((link && link.rawText) || ''), seen = (link && link.seen) || {};
    if (seen.off) {
      return { code: 'LISTENER_OFF', detail: 'This SpiderBridge is already set up and knows a controller, so it deliberately does not ' +
        'listen for USB commands (that memory is needed for the controller\'s connection). Change Wi-Fi and the hotspot password in its ' +
        'web interface instead (join its hotspot and open http://192.168.10.1). Or install the firmware again with "Erase the board" ' +
        'ticked: that removes the settings and the known controllers, and Quick Connect then works.' };
    }
    if (seen.ready || seen.app) {
      return { code: 'NO_ANSWER', detail: 'The board runs SpiderBridge but did not answer. It always listens, so this is unusual: press its ' +
        'reset button, wait about 20 seconds and try again. If it keeps happening, open "Health & restarts" in the installer window.' };
    }
    if (t.replace(/\s+/g, '').length > 20) {
      return { code: 'FOREIGN_FIRMWARE', detail: 'The board is running other firmware (it printed text, but nothing from SpiderBridge). ' +
        'Install the SpiderBridge firmware first.' };
    }
    return { code: 'NO_ANSWER', detail: 'The board did not answer and printed nothing at 115200 baud. Check that you picked the right port and ' +
      'use a USB data cable, press the board\'s reset button and try again.' };
  }

  // 15-character hotspot password with the same alphabet and rules as the firmware
  // (no look-alike characters; at least one lower, upper, digit and symbol).
  function randomHotspotPassword(rng) {
    var SET = 'abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789#!';
    var get = rng || function (n) { var a = new Uint32Array(1); var out = []; for (var i = 0; i < n; i++) { crypto.getRandomValues(a); out.push(a[0]); } return out; };
    for (;;) {
      var r = get(15), s = '';
      for (var i = 0; i < 15; i++) s += SET.charAt(r[i] % SET.length);
      if (/[a-z]/.test(s) && /[A-Z]/.test(s) && /[0-9]/.test(s) && /[#!]/.test(s)) return s;
    }
  }

  function enc(strings) {
    var te = new TextEncoder(), out = [];
    strings.forEach(function (s) {
      var b = te.encode(s);
      if (b.length > 255) throw new Error('INVALID_RPC_PACKET');
      out.push(b.length);
      for (var i = 0; i < b.length; i++) out.push(b[i]);
    });
    return out;
  }

  // ---- Improv serial client (own implementation, same wire format) --------
  function ImprovLink(port) {
    this.port = port;
    this.buf = [];
    this.pending = null;
    this.state = 0;
    this.reader = null;
    this.writer = null;
    this.closed = false;
    this.rawText = '';        // the last few KB the board printed, for diagnosing silence
    this.seen = { off: false, ready: false, app: false };   // key log lines, remembered for good
    this.tail = '';
  }

  ImprovLink.prototype.start = function () {
    var self = this;
    this.writer = this.port.writable.getWriter();
    this.reader = this.port.readable.getReader();
    this.loop = (async function () {
      try {
        for (;;) {
          var r = await self.reader.read();
          if (r.done) break;
          self._feed(r.value);
        }
      } catch (e) { /* port closed */ }
      finally { try { self.reader.releaseLock(); } catch (e) {} }
    })();
  };

  // Packet: "IMPROV" 01 type len data... checksum, then '\n'. Text (the firmware's log lines)
  // is mixed in the same stream, so re-synchronise on the header after every byte.
  ImprovLink.prototype._feed = function (chunk) {
    var s = '';
    for (var k = 0; k < chunk.length; k++) s += String.fromCharCode(chunk[k]);
    this.rawText = (this.rawText + s).slice(-6000);
    var win = this.tail + s;                       // a line may be split across two chunks
    if (/Improv\) off: bridge configured and controllers known/.test(win)) this.seen.off = true;
    if (/Improv\) ready on the USB port/.test(win)) this.seen.ready = true;
    if (/SpiderBridge-ESP32 v2/.test(win)) this.seen.app = true;
    this.tail = win.slice(-100);
    for (var i = 0; i < chunk.length; i++) {
      this.buf.push(chunk[i]);
      for (;;) {
        var b = this.buf, k = -1;
        for (var j = 0; j + 6 <= b.length; j++) {
          if (b[j] === 73 && String.fromCharCode.apply(null, b.slice(j, j + 6)) === 'IMPROV') { k = j; break; }
        }
        if (k < 0) { if (b.length > 6) this.buf = b.slice(b.length - 5); break; }
        if (k > 0) { this.buf = b = b.slice(k); }
        if (b.length < 9) break;
        var total = 9 + b[8] + 1;
        if (b.length < total) break;
        var frame = b.slice(0, total);
        this.buf = b.slice(total);
        this._packet(frame);
      }
    }
  };

  ImprovLink.prototype._packet = function (f) {
    var sum = 0;
    for (var i = 0; i < f.length - 1; i++) sum += f[i];
    if ((sum & 255) !== f[f.length - 1] || f[6] !== 1) return;
    var type = f[7], len = f[8], data = f.slice(9, 9 + len);
    if (type === TYPE.STATE) { this.state = data[0]; return; }
    if (type === TYPE.ERROR) {
      if (data[0] && this.pending) this.pending.reject(Object.assign(new Error(IMPROV_ERR[data[0]] || 'UNKNOWN_ERROR'), { improv: IMPROV_ERR[data[0]] || 'UNKNOWN_ERROR' }));
      return;
    }
    if (type === TYPE.RESULT && this.pending && data[0] === this.pending.cmd) {
      var out = [], td = new TextDecoder('utf-8'), p = 2, total = 2 + data[1];
      while (p < total) { out.push(td.decode(new Uint8Array(data.slice(p + 1, p + 1 + data[p])))); p += 1 + data[p]; }
      var pend = this.pending;
      if (pend.multi) { if (out.length) pend.rows.push(out); else pend.resolve(pend.rows); }
      else pend.resolve(out);
    }
  };

  ImprovLink.prototype.call = function (cmd, args, timeout, multi) {
    var self = this;
    return new Promise(function (resolve, reject) {
      var timer = setTimeout(function () { self.pending = null; reject(Object.assign(new Error('TIMEOUT'), { improv: 'TIMEOUT' })); }, timeout || 8000);
      var done = function (fn) { return function (v) { clearTimeout(timer); self.pending = null; fn(v); }; };
      self.pending = { cmd: cmd, multi: !!multi, rows: [], resolve: done(resolve), reject: done(reject) };
      var data = [cmd, args.length].concat(args);
      var pkt = [73, 77, 80, 82, 79, 86, 1, TYPE.RPC, data.length].concat(data);
      var s = 0; pkt.forEach(function (x) { s += x; });
      pkt.push(s & 255, 10);
      self.writer.write(new Uint8Array(pkt)).catch(function (e) { self.pending && self.pending.reject(e); });
    });
  };

  ImprovLink.prototype.close = async function () {
    if (this.closed) return; this.closed = true;
    try { await this.reader.cancel(); } catch (e) {}
    try { await this.loop; } catch (e) {}
    try { this.writer.releaseLock(); } catch (e) {}
  };

  // ---- flashing -----------------------------------------------------------
  async function loadEsptool() {
    try { return await import(ESPTOOL_URL); }
    catch (e) {
      // The pinned bundle is a plain ES module as well.
      return await import(ESPTOOL_FALLBACK);
    }
  }

  async function fetchManifest(url) {
    var r = await fetch(url, { cache: 'no-store' });
    if (!r.ok) throw new Error('Could not load the firmware list (HTTP ' + r.status + ').');
    return r.json();
  }

  async function fetchBuild(manifestUrl, chipFamily, onNote) {
    var m = await fetchManifest(manifestUrl);
    var build = (m.builds || []).filter(function (b) { return b.chipFamily === chipFamily; })[0];
    if (!build) {
      var have = (m.builds || []).map(function (b) { return b.chipFamily; }).join(', ');
      throw new Error('Your board is a ' + chipFamily + '. This firmware is built for: ' + have + '. Nothing was changed.');
    }
    var parts = [];
    for (var i = 0; i < build.parts.length; i++) {
      var url = new URL(build.parts[i].path, new URL(manifestUrl, location.href)).href;
      if (onNote) onNote('Downloading ' + build.parts[i].path + '…');
      var r = await fetch(url, { cache: 'no-store' });
      if (!r.ok) throw new Error('Downloading the firmware failed (HTTP ' + r.status + ').');
      var bytes = new Uint8Array(await r.arrayBuffer());
      parts.push({ data: bytes, address: build.parts[i].offset });
    }
    return { version: m.version || '', parts: parts };
  }

  // esptool-js 0.6: ESPLoader.main() opens the port itself (Transport.connect), so the port must be
  // closed when it is called; disconnect() closes it again.
  async function flash(port, manifestUrl, opts, step) {
    var lib = await loadEsptool();
    var transport = new lib.Transport(port, false);
    var loader = new lib.ESPLoader({
      transport: transport, baudrate: SERIAL_BAUD,
      terminal: { clean: function () {}, writeLine: function (s) { opts.log && opts.log(s); }, write: function (s) { opts.log && opts.log(s); } }
    });
    var chip;
    try {
      step('Connecting to the chip… (hold the BOOT button now if this takes long)');
      await loader.main();
      chip = loader.chip.CHIP_NAME;
    } catch (e) {
      try { await transport.disconnect(); } catch (x) {}
      var err = new Error('The board did not enter download mode. Hold the BOOT button, press Start again, and release BOOT as soon as "Erasing" appears.');
      err.boot = true; err.cause = e; throw err;
    }
    try {
      step('Found ' + chip + '. Loading the firmware…');
      var fw = await fetchBuild(manifestUrl, chip, function (n) { step(n); });
      if (opts.erase) { step('Erasing the board…'); await loader.eraseFlash(); }
      var total = fw.parts.reduce(function (s, p) { return s + p.data.length; }, 0), done = 0;
      step('Writing the firmware… 0%', 0);
      await loader.writeFlash({
        fileArray: fw.parts, flashSize: 'keep', flashMode: 'keep', flashFreq: 'keep', eraseAll: false, compress: true,
        reportProgress: function (idx, written, tot) {
          var pct = Math.floor((done + (written / tot) * fw.parts[idx].data.length) / total * 100);
          step('Writing the firmware… ' + pct + '%', pct);
          if (written === tot) done += fw.parts[idx].data.length;
        }
      });
      step('Restarting the board…', 100);
      await loader.after('hard_reset');
    } finally {
      try { await transport.disconnect(); } catch (x) {}
    }
    return { chip: chip, version: fw && fw.version };
  }

  // ---- the wizard dialog --------------------------------------------------
  var STEPS_FULL = ['Board', 'Wi-Fi', 'Install', 'Done'];
  var STEPS_SET = ['Board', 'Wi-Fi', 'Setup', 'Done'];

  function open(options) {
    options = options || {};
    var manifestUrl = options.manifestUrl;
    var flashMode = options.mode !== 'setup';   // false: the board already runs SpiderBridge, do not flash
    var d = document.createElement('dialog');
    d.className = 'sbo sbq';
    // The live console must let go of the port while the wizard uses it.
    if (window.SBConsole) window.SBConsole.pause();
    var h2 = el('h2', null, 'Quick Connect'); var head = el('div', 'sbo-head'); head.appendChild(h2);
    var bar = el('div', 'sbq-steps'); head.appendChild(bar);
    function paintBar() { bar.textContent = ''; (flashMode ? STEPS_FULL : STEPS_SET).forEach(function (s, i) { bar.appendChild(el('span', null, (i + 1) + ' ' + s)); }); h2.textContent = flashMode ? 'Quick Connect — install & set up' : 'Quick Connect — set up (no flashing)'; }
    paintBar();
    var body = el('div', 'sbo-body'), actions = el('div', 'sbo-actions');
    d.appendChild(head); d.appendChild(body); d.appendChild(actions);
    var busy = false, port = null;
    d.addEventListener('cancel', function (e) { if (busy) e.preventDefault(); });
    d.addEventListener('close', function () { d.remove(); if (window.SBConsole) window.SBConsole.resume(); });
    document.body.appendChild(d);

    function mark(n) { Array.prototype.forEach.call(bar.children, function (c, i) { c.className = i < n ? 'done' : i === n ? 'cur' : ''; }); }
    function btn(label, primary, fn) { var b = el('button', primary ? 'primary' : '', label); b.type = 'button'; b.addEventListener('click', fn); return b; }
    function acts() { actions.textContent = ''; Array.prototype.slice.call(arguments).forEach(function (b) { actions.appendChild(b); }); }
    function msg(kind, text) { var m = el('div', 'sbo-msg ' + kind, text); body.appendChild(m); return m; }

    // -- step 1/2: form
    function form(prev) {
      busy = false; mark(0); body.textContent = '';
      var sw = el('label', 'chk'); var swBox = el('input'); swBox.type = 'checkbox'; swBox.id = 'sbq-flash'; swBox.checked = flashMode;
      sw.appendChild(swBox); sw.appendChild(document.createTextNode('Install the newest firmware first (new board or update)'));
      var swNote = el('p', 'sbq-note');
      function paintIntro() {
        swNote.textContent = flashMode
          ? 'Installs the newest SpiderBridge firmware, joins your home Wi-Fi and gives the bridge\'s own hotspot a new random password — in one go.'
          : 'The board already runs SpiderBridge firmware: nothing is flashed. It joins your home Wi-Fi, gets a new random hotspot password and can connect your GGS controller. The board is restarted first so that it listens for these commands.';
      }
      body.appendChild(sw); body.appendChild(swNote);
      var lblNet = el('label', null, 'Home Wi-Fi name'); lblNet.setAttribute('for', 'sbq-ssid');
      var ssid = el('input'); ssid.type = 'text'; ssid.id = 'sbq-ssid'; ssid.maxLength = 32; ssid.autocomplete = 'off'; ssid.spellcheck = false;
      var lblPw = el('label', null, 'Home Wi-Fi password'); lblPw.setAttribute('for', 'sbq-pw');
      var pw = el('input'); pw.type = 'password'; pw.id = 'sbq-pw'; pw.maxLength = 64; pw.autocomplete = 'off';
      var show = el('label', 'chk'); var showBox = el('input'); showBox.type = 'checkbox'; show.appendChild(showBox); show.appendChild(document.createTextNode('Show password'));
      showBox.addEventListener('change', function () { pw.type = showBox.checked ? 'text' : 'password'; });
      var ble = el('label', 'chk'); var bleBox = el('input'); bleBox.type = 'checkbox'; bleBox.id = 'sbq-ble'; bleBox.checked = true;
      ble.appendChild(bleBox); ble.appendChild(document.createTextNode('On the first start, search for Spider Farmer GGS controllers over Bluetooth and connect them to the bridge'));
      var bleNote = el('p', 'sbq-note', 'The controllers stay visible over Bluetooth, so you can still connect your phone and the Spider Farmer app afterwards. ' +
        'The controller must be switched on, close to the board, and not connected to a phone right now.');
      var erase = el('label', 'chk'); var eraseBox = el('input'); eraseBox.type = 'checkbox'; eraseBox.checked = true;
      erase.appendChild(eraseBox); erase.appendChild(document.createTextNode('Erase the board first (recommended for a new board; removes old settings)'));
      var bleLabel = ble.lastChild;
      function paintMode() {
        flashMode = swBox.checked;
        erase.style.display = flashMode ? '' : 'none';
        bleLabel.nodeValue = flashMode ? 'On the first start, search for Spider Farmer GGS controllers over Bluetooth and connect them to the bridge'
                                       : 'After the restart, search for Spider Farmer GGS controllers over Bluetooth and connect them to the bridge';
        go.textContent = flashMode ? 'Select board & start' : 'Select board & set up';
        paintBar(); paintIntro();
      }
      var err = el('div', 'sbo-msg err'); err.style.display = 'none';
      [lblNet, ssid, lblPw, pw, show, ble, bleNote, erase, err].forEach(function (n) { body.appendChild(n); });
      if (prev) { ssid.value = prev.ssid; pw.value = prev.pw; bleBox.checked = prev.ble; eraseBox.checked = prev.erase; }
      var go;

      function submit() {
        var te = new TextEncoder(), p = '';
        if (!ssid.value.trim()) p = 'Enter the name of your home Wi-Fi.';
        else if (te.encode(ssid.value.trim()).length > 32) p = 'The Wi-Fi name is longer than 32 bytes.';
        else if (pw.value.length < 8 || te.encode(pw.value).length > 64) p = 'A Wi-Fi password has 8 to 64 characters.';
        if (p) { err.textContent = p; err.style.display = ''; return; }
        run({ ssid: ssid.value.trim(), pw: pw.value, ble: bleBox.checked, erase: eraseBox.checked, flash: flashMode });
      }
      pw.addEventListener('keydown', function (e) { if (e.key === 'Enter') submit(); });
      go = btn('Select board & start', true, submit);
      swBox.addEventListener('change', paintMode);
      acts(btn('Cancel', false, function () { d.close(); }), go);
      paintMode();
    }

    // -- step 3: do it
    function run(v) {
      busy = true; mark(1); body.textContent = ''; acts(btn('Working…', false, function () {}));
      actions.firstChild.disabled = true;
      var line = el('p'); var spin = el('span', 'sbo-spin'); var txt = document.createTextNode(''); line.appendChild(spin); line.appendChild(txt);
      var bar2 = el('progress'); bar2.max = 100; bar2.value = 0; bar2.style.width = '100%'; bar2.style.display = 'none';
      var log = el('pre', 'sbq-log'); log.hidden = true;
      v.__log = log;
      body.appendChild(line); body.appendChild(bar2);
      function step(t, pct) { txt.nodeValue = t; if (pct != null) { bar2.style.display = ''; bar2.value = pct; } }
      var password = randomHotspotPassword();

      (async function () {
        try {
          if (!port) {
            step('Choose the board in the window Chrome opens…');
            port = await navigator.serial.requestPort();
          }
          mark(2);
          var res;
          if (v.flash) {
            res = await (options._flash || flash)(port, manifestUrl, { erase: v.erase, log: function (s) { log.textContent += s + '\n'; } }, step);
            bar2.style.display = 'none';
            step('Waiting for the new firmware to start… (about 10 seconds)');
            // flash() closed the transport; reopen the port for Improv
            await sleep(1500);
            await port.open({ baudRate: SERIAL_BAUD, bufferSize: 8192 });
          } else {
            // No flashing. A bridge only listens for USB commands for a few minutes after power-up,
            // so restart it first (the serial control lines pulse the reset pin on most boards).
            step('Restarting the board so that it listens for commands…');
            await port.open({ baudRate: SERIAL_BAUD, bufferSize: 8192 });
            try { await port.setSignals({ dataTerminalReady: false, requestToSend: true }); await sleep(150); await port.setSignals({ requestToSend: false }); } catch (e) { /* no control lines */ }
            res = { chip: '', version: '' };
          }
          var link = new ImprovLink(port); link.start();
          try {
            try { await waitForImprov(link, 40000); }
            catch (e) { throw Object.assign(new Error(e.message), diagnose(link), { improv: undefined }); }
            if (!v.flash) {
              step('Checking the firmware…');
              var info = await link.call(CMD.INFO, [], 4000);
              res.chip = info[2] || 'ESP32'; res.version = info[1] || '';
              if (info[0] !== 'SpiderBridge') throw Object.assign(new Error('NOT_SPIDERBRIDGE'), { improv: 'NOT_SPIDERBRIDGE' });
            }
            step('Sending the Wi-Fi settings and the new hotspot password… (up to 30 seconds)');
            var r = await link.call(CMD.WIFI_AP, enc([v.ssid, v.pw, password, v.ble ? '1' : '0']), 45000);
            // the board restarts about 2.5 s after answering
            await link.close(); try { await port.close(); } catch (e) {}
            var result = { flashed: v.flash, chip: res.chip, version: res.version, url: r[0] || '', apSsid: r[1] || 'SpiderBridge', apPass: r[2] || password, ble: v.ble, ssid: v.ssid };
            step('The board is restarting…');
            await sleep(options._settleMs != null ? options._settleMs : 9000);
            done(result);
          } catch (e) {
            await link.close(); try { await port.close(); } catch (x) {}
            throw e;
          }
        } catch (e) { failed(e, v); }
      })();
    }

    async function waitForImprov(link, ms) {
      var t0 = Date.now(), last;
      while (Date.now() - t0 < ms) {
        if (link.seen.off) break;      // the firmware just said it will not listen: no point waiting
        // The board printed its boot text, none of it from SpiderBridge, and has been quiet since: other
        // firmware. SpiderBridge announces itself within ~3 s of a reset, so 15 s of foreign text is enough.
        if (Date.now() - t0 > 15000 && !link.seen.app && !link.seen.ready && link.rawText.replace(/\s+/g, '').length > 20) break;
        try { await link.call(CMD.STATE, [], 1500); return; } catch (e) { last = e; await sleep(800); }
      }
      throw Object.assign(new Error('The new firmware did not answer over USB. Press the board\'s reset button and try Quick Connect again.'), { improv: 'TIMEOUT' });
    }

    function failed(e, v) {
      busy = false; body.textContent = '';
      var p0 = port; port = null;
      if (p0 && p0.readable) { try { p0.close().catch(function () {}); } catch (x) {} }
      var text = e && e.boot ? e.message : e && e.name === 'NotFoundError' ? 'No board was selected.' : explain(e);
      body.appendChild(el('div', 'sbo-msg err', text));
      if (v && v.__log && v.__log.textContent.trim()) {
        var det = document.createElement('details'); det.appendChild(el('summary', null, 'Technical details'));
        v.__log.hidden = false; det.appendChild(v.__log); body.appendChild(det);
      }
      if (e && e.boot) body.appendChild(el('p', 'sbq-note', 'Some boards enter download mode on their own; this one needs the BOOT button held while the connection starts.'));
      var again = btn('Try again', true, function () { form(v); });
      var code = e && (e.code || e.improv);
      if (!v.flash && code && OFFER_INSTALL[code]) {
        var inst = btn(code === 'LISTENER_OFF' ? 'Erase, install firmware and continue' : 'Install firmware and continue', true,
          function () {
            v.flash = true; flashMode = true;
            // Wiping the board is only done where the button says so; otherwise keep the user's settings.
            v.erase = code === 'LISTENER_OFF';
            paintBar(); h2.textContent = 'Quick Connect \u2014 install & set up';
            run(v);
          });
        again.className = '';
        acts(btn('Close', false, function () { d.close(); }), again, inst);
      } else {
        acts(btn('Close', false, function () { d.close(); }), again);
      }
    }

    // -- step 4: result
    function done(r) {
      busy = false; mark(3); body.textContent = '';
      body.appendChild(el('div', 'sbo-msg ok', 'Done. ' + (r.flashed ? (r.chip + ' is running the new firmware and ') : 'The board ') + 'joined "' + r.ssid + '".'));
      var kv = function (k, v) { var p = el('p', 'sbo-kv'); p.appendChild(document.createTextNode(k + ' ')); var b = el('b'); if (v && v.nodeType) b.appendChild(v); else b.textContent = v; p.appendChild(b); body.appendChild(p); };
      if (/^http:\/\/\d{1,3}(\.\d{1,3}){3}\/?$/.test(r.url)) { var a = el('a', null, r.url); a.href = r.url; a.target = '_blank'; a.rel = 'noopener'; a.style.color = '#8ab4f8'; kv('Web interface:', a); }
      body.appendChild(el('p', 'sbo-kv', 'Hotspot "' + r.apSsid + '" — new password (shown only now, write it down):'));
      body.appendChild(el('div', 'sbo-secret', r.apPass));
      if (r.ble) {
        body.appendChild(el('div', 'sbo-msg warn', 'Bluetooth: after the restart the bridge searches for your GGS controller (about 20 seconds) and connects it to the hotspot. ' +
          'The controller stays visible over Bluetooth, so you can still connect your phone and the Spider Farmer app. ' +
          'No controller found? Switch it on close to the board, make sure no phone is connected to it, and use "Scan" on the bridge\'s Control page.'));
      }
      var copyBtn = btn('Copy password', false, function () { if (navigator.clipboard) navigator.clipboard.writeText(r.apPass).then(function () { copyBtn.textContent = 'Copied'; }); });
      acts(copyBtn, btn('Close', true, function () { d.close(); }));
      if (options.onDone) options.onDone(r);
    }

    form();
    d.showModal();
    return d;
  }

  window.SBQuickConnect = {
    open: open,
    ImprovLink: ImprovLink,
    _test: { randomHotspotPassword: randomHotspotPassword, ImprovLink: ImprovLink, enc: enc, CMD: CMD, diagnose: diagnose }
  };
})();
