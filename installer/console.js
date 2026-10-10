/*
 * SpiderBridge live console.
 *
 * A console that stays connected: it reads the board's log continuously and, when the connection
 * drops (a reset after flashing, a crash, the USB cable pulled and plugged back in), it reconnects by
 * itself as soon as the board is back. It never needs to be restarted by hand.
 *
 * A serial port can be used by only one reader at a time, so the console steps aside whenever the
 * installer window or the Quick Connect wizard needs the port (pause) and takes it back afterwards
 * (resume). Both are wired up in index.html.
 *
 * Restart and Health use the same USB commands as the installer window (Improv extensions 0x44 and
 * 0x45, see firmware/ggs/main/improv_serial.c); the replies are picked out of the same stream.
 *
 * Exposes window.SBConsole = { mount(root), pause(), resume() }.
 */
(function () {
  'use strict';

  var BAUD = 115200;
  var MAX_LINES = 400;
  var POLL_MS = 1500;                 // how often a lost board is looked for
  var SV_TEXT = ['', 'a task stopped responding', 'memory ran low', 'a Bluetooth step stalled', 'restart requested', 'the uplink was down'];

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  function Console(root) {
    this.root = root;
    this.port = null;
    this.state = 'idle';              // idle | connecting | connected | lost | paused
    this.want = false;                // the user asked for a console: keep it alive
    this.paused = 0;                  // depth of pause() calls
    this.lines = [];
    this.partial = '';
    this.run = 0;                     // generation: a stale read loop must not touch the new one
    this.link = null;
    this.build();
    var self = this;
    if ('serial' in navigator) {
      // The board comes back after a reset or replug: Chrome fires this for ports it has access to.
      navigator.serial.addEventListener('connect', function () { self.kick(); });
      navigator.serial.addEventListener('disconnect', function () { self.lost('USB cable unplugged'); });
    }
  }

  Console.prototype.build = function () {
    var self = this;
    var bar = el('div', 'sbc-bar');
    this.status = el('span', 'sbc-status', 'Not connected');
    this.btnConnect = el('button', 'act', 'Connect console');
    this.btnConnect.type = 'button';
    this.btnConnect.addEventListener('click', function () { self.connect(true); });
    this.btnRestart = el('button', 'act alt', 'Restart board');
    this.btnRestart.type = 'button';
    this.btnRestart.addEventListener('click', function () { self.restart(); });
    this.btnHealth = el('button', 'act alt', 'Health');
    this.btnHealth.type = 'button';
    this.btnHealth.addEventListener('click', function () { self.health(); });
    this.btnClear = el('button', 'act alt', 'Clear');
    this.btnClear.type = 'button';
    this.btnClear.addEventListener('click', function () { self.lines = []; self.partial = ''; self.paint(); });
    this.btnSave = el('button', 'act alt', 'Download log');
    this.btnSave.type = 'button';
    this.btnSave.addEventListener('click', function () { self.save(); });
    [this.btnConnect, this.btnRestart, this.btnHealth, this.btnClear, this.btnSave, this.status].forEach(function (n) { bar.appendChild(n); });
    this.out = el('pre', 'sbq-log sbc-out');
    this.out.setAttribute('aria-live', 'off');
    this.report = el('div', 'sbc-report');
    this.root.appendChild(bar);
    this.root.appendChild(this.report);
    this.root.appendChild(this.out);
    this.setButtons();
  };

  Console.prototype.setState = function (s, text) {
    this.state = s;
    this.status.textContent = text;
    this.status.className = 'sbc-status ' + s;
    this.setButtons();
  };

  Console.prototype.setButtons = function () {
    var live = this.state === 'connected';
    this.btnRestart.disabled = !live;
    this.btnHealth.disabled = !live;
    this.btnConnect.textContent = this.want ? 'Disconnect console' : 'Connect console';
  };

  // ---- text ---------------------------------------------------------------
  // An Improv packet is "IMPROV", 3 header bytes, a length, that many data bytes, a checksum and a newline.
  // It is binary and belongs to the commands, not to the log. Removed on the BYTES, before decoding, so a
  // packet split across two reads, or a byte that is not valid text, cannot leak into the log.
  Console.prototype.stripPackets = function (bytes) {
    var carry = this.carry || [];
    var b = carry.length ? Array.prototype.slice.call(carry).concat(Array.prototype.slice.call(bytes)) : bytes;
    var out = [], i = 0, n = b.length;
    this.carry = null;
    while (i < n) {
      // is "IMPROV" starting here?
      if (b[i] === 73 && i + 6 <= n && b[i + 1] === 77 && b[i + 2] === 80 && b[i + 3] === 82 && b[i + 4] === 79 && b[i + 5] === 86) {
        if (i + 9 > n) { this.carry = Array.prototype.slice.call(b, i); break; }     // header not complete yet
        var end = i + 9 + b[i + 8] + 2;
        if (end > n) { this.carry = Array.prototype.slice.call(b, i); break; }       // packet not complete yet
        i = end;
        continue;
      }
      // "IMPRO" cut off right at the end of a read: wait for the next read
      if (b[i] === 73 && n - i < 6 && 'IMPROV'.indexOf(String.fromCharCode.apply(null, Array.prototype.slice.call(b, i))) === 0) {
        this.carry = Array.prototype.slice.call(b, i);
        break;
      }
      out.push(b[i]);
      i++;
    }
    return new Uint8Array(out);
  };

  Console.prototype.add = function (text) {
    text = text.replace(/\x1b\[[0-9;]*m/g, '').replace(/\r/g, '');
    var parts = (this.partial + text).split('\n');
    this.partial = parts.pop();
    for (var i = 0; i < parts.length; i++) this.lines.push(parts[i]);
    if (this.lines.length > MAX_LINES) this.lines = this.lines.slice(-MAX_LINES);
    this.schedulePaint();
  };

  Console.prototype.note = function (text) { this.add('\n--- ' + text + ' ---\n'); };

  Console.prototype.schedulePaint = function () {
    var self = this;
    if (this._painting) return;
    this._painting = true;
    setTimeout(function () { self._painting = false; self.paint(); }, 100);
  };

  Console.prototype.paint = function () {
    var stick = this.out.scrollTop + this.out.clientHeight >= this.out.scrollHeight - 30;
    this.out.textContent = this.lines.join('\n') + (this.partial ? '\n' + this.partial : '');
    if (stick) this.out.scrollTop = this.out.scrollHeight;
  };

  Console.prototype.save = function () {
    var blob = new Blob([this.lines.join('\n') + '\n'], { type: 'text/plain' });
    var a = el('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'spiderbridge-console.txt';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(a.href); }, 2000);
  };

  // ---- connection ---------------------------------------------------------
  // The user clicks once; from then on the console keeps itself connected.
  Console.prototype.connect = function (fromClick) {
    if (fromClick && this.want) { this.disconnect(); return; }
    if (fromClick) this.want = true;
    this.setButtons();
    return this.open(fromClick);
  };

  Console.prototype.disconnect = function () {
    this.want = false;
    this.close();
    this.setState('idle', 'Not connected');
  };

  Console.prototype.kick = function () {
    if (this.want && this.state !== 'connected' && this.state !== 'connecting' && !this.paused) this.open(false);
  };

  Console.prototype.pickPort = async function (allowPrompt) {
    var have = await navigator.serial.getPorts();
    if (this.port && have.indexOf(this.port) >= 0) return this.port;
    if (have.length) return have[0];
    if (!allowPrompt) return null;
    return navigator.serial.requestPort();      // the browser's port window; needs the user's click
  };

  Console.prototype.open = async function (allowPrompt) {
    if (this.paused || this.state === 'connecting' || this.state === 'connected') return;
    var gen = ++this.run;
    this.setState('connecting', 'Connecting…');
    var port;
    try { port = await this.pickPort(allowPrompt); }
    catch (e) { this.want = false; this.setState('idle', e && e.name === 'NotFoundError' ? 'No port chosen' : 'Not connected'); return; }
    if (!port) { this.setState('lost', 'Waiting for the board…'); this.poll(gen); return; }
    try {
      await port.open({ baudRate: BAUD, bufferSize: 8192 });
      // Both control lines high = the board runs. (Chrome asserts them on open; made explicit so a
      // port that came up with another state cannot hold the board in reset.)
      try { await port.setSignals({ dataTerminalReady: true, requestToSend: true }); } catch (e) { /* no control lines */ }
    } catch (e) {
      // Busy (another tab or program) or not ready yet after a replug: try again shortly.
      this.setState('lost', 'Waiting for the port… (' + (e && e.name === 'InvalidStateError' ? 'already open' : 'busy or unplugged') + ')');
      this.poll(gen);
      return;
    }
    if (gen !== this.run) { try { await port.close(); } catch (e) {} return; }
    this.port = port;
    this.setState('connected', 'Connected');
    this.note('console connected');
    this.attach(port, gen);
  };

  Console.prototype.attach = function (port, gen) {
    var self = this;
    var reader = port.readable.getReader();
    var writer = port.writable.getWriter();
    var dec = new TextDecoder('utf-8', { fatal: false });
    this.reader = reader; this.writer = writer;
    // Improv replies are picked out of the same byte stream by the wizard's parser.
    var QC = window.SBQuickConnect;
    this.link = null;
    if (QC && QC.ImprovLink) {
      var l = new QC.ImprovLink({});
      l.writer = { write: function (b) { return writer.write(b); } };
      this.link = l;
    }
    (async function () {
      try {
        for (;;) {
          var r = await reader.read();
          if (r.done) break;
          if (gen !== self.run) break;
          if (self.link) self.link._feed(r.value);
          self.add(dec.decode(self.stripPackets(r.value), { stream: true }));
        }
      } catch (e) { /* the port went away */ }
      try { reader.releaseLock(); } catch (e) {}
      try { writer.releaseLock(); } catch (e) {}
      if (gen === self.run && self.state === 'connected') self.lost('connection ended');
    })();
  };

  Console.prototype.close = async function () {
    this.run++;
    var port = this.port, reader = this.reader;
    this.reader = this.writer = null; this.link = null;
    try { if (reader) await reader.cancel(); } catch (e) {}
    // Give the read loop a moment to release its locks before closing the port.
    await sleep(60);
    try { if (port) await port.close(); } catch (e) {}
  };

  Console.prototype.lost = function (why) {
    if (this.state === 'paused' || this.state === 'idle') return;
    this.note('lost: ' + why);
    var gen = this.run;
    this.close().then(function () {});
    if (!this.want) { this.setState('idle', 'Not connected'); return; }
    this.setState('lost', 'Reconnecting… (' + why + ')');
    this.poll(this.run);
  };

  // Look for the board until it is back. Cheap: one getPorts() call every 1.5 s.
  Console.prototype.poll = function (gen) {
    var self = this;
    setTimeout(function () {
      if (!self.want || self.paused || self.state === 'connected' || self.state === 'connecting') return;
      if (self.state !== 'lost') return;
      self.state = 'idle';           // let open() through
      self.open(false).then(function () { if (self.state === 'idle' && self.want) { self.setState('lost', 'Waiting for the board…'); self.poll(self.run); } });
    }, POLL_MS);
  };

  // ---- hand-over to the installer window and the wizard -------------------
  Console.prototype.pause = async function () {
    this.paused++;
    if (this.paused > 1) return;
    var was = this.state;
    if (was === 'connected' || was === 'connecting' || was === 'lost') {
      await this.close();
      this.note('console paused: the port is in use by the installer');
    }
    this.setState('paused', this.want ? 'Paused while the installer uses the port' : 'Not connected');
  };

  Console.prototype.resume = function () {
    if (this.paused === 0) return;
    this.paused--;
    if (this.paused > 0) return;
    if (this.want) { this.state = 'idle'; this.open(false); }
    else this.setState('idle', 'Not connected');
  };

  // ---- commands over the same stream --------------------------------------
  Console.prototype.rpc = function (cmd, timeout) {
    if (!this.link) return Promise.reject(new Error('The console is not connected.'));
    return this.link.call(cmd, [], timeout || 6000);
  };

  Console.prototype.restart = function () {
    var self = this;
    this.report.textContent = 'Restarting the board…';
    this.rpc(0x44, 6000).then(function () {
      self.report.textContent = 'The board is restarting. The console stays connected and shows the start.';
    }).catch(function (e) { self.report.textContent = 'Could not restart it: ' + ((e && e.improv) || (e && e.message) || e); });
  };

  Console.prototype.health = function () {
    var self = this;
    this.report.textContent = 'Reading…';
    this.rpc(0x45, 6000).then(function (r) {
      if (!r || r.length < 11) throw new Error('unexpected answer');
      var up = parseInt(r[7], 10) || 0;
      var upTxt = up >= 3600 ? Math.floor(up / 3600) + ' h ' + Math.floor((up % 3600) / 60) + ' min' : up >= 60 ? Math.floor(up / 60) + ' min ' + (up % 60) + ' s' : up + ' s';
      var why = parseInt(r[5], 10) || 0;
      var parts = [];
      if (r[4] === '1') parts.push('SAFE MODE: the last ' + r[2] + ' starts ended in a crash. Only hotspot, web interface and this console run.');
      parts.push('This start: ' + r[0] + ' · up ' + upTxt + ' · start #' + r[1] + ' · crashes so far ' + r[3] + (+r[2] ? ' · ' + r[2] + ' in a row' : ''));
      if (why) parts.push('Last self-restart: ' + (SV_TEXT[why] || 'internal check') + (r[6] ? ' (' + r[6] + ')' : ''));
      parts.push('Memory ' + Math.round(r[8] / 1024) + ' KB free, lowest ' + Math.round(r[9] / 1024) + ' KB');
      self.report.textContent = parts.join('  |  ');
    }).catch(function (e) {
      var k = (e && e.improv) || (e && e.message) || String(e);
      self.report.textContent = k === 'UNKNOWN_RPC_COMMAND' ? 'This firmware is older than the health report. Install the current firmware.' : 'Could not read it: ' + k;
    });
  };

  var instance = null;
  window.SBConsole = {
    mount: function (root) { instance = new Console(root); return instance; },
    pause: function () { return instance ? instance.pause() : Promise.resolve(); },
    resume: function () { if (instance) instance.resume(); },
    _instance: function () { return instance; }
  };
})();
