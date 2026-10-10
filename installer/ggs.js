/*
 * "Send Wi-Fi to GGS Controller": the logic, without any page code, so it can be tested in Node.
 *
 * The bridge does the Bluetooth work in a boot of its own (firmware/ggs/main/ggs_ble.c): a search or a
 * send restarts it into a Bluetooth-only start, which is silent on USB for 10 to 120 seconds, and then back
 * into normal operation. So a request is "ask, then wait for the board to restart twice and answer again,
 * then read the outcome". The restart counter (diagnostics, command 0x45) tells restarts apart from silence.
 *
 * USB commands used (SpiderBridge extensions of Improv Wi-Fi Serial, firmware/ggs/main/improv_serial.c):
 *   0x43  Bluetooth status: armed flag, last text, number found, one "address|name|rssi|flags" per controller
 *   0x45  diagnostics (the start counter is string 1)
 *   0x47  search for controllers again            -> "1" started, "0" busy
 *   0x48  send the hotspot Wi-Fi to [address]     -> "1" started, "0" busy; only a controller the last search found
 *   0x49  outcome of the last send                -> [state, address, text, pending]; state 2 = accepted, 3 = failed
 *
 * Everything the page needs goes through io = { rpc(cmd, argBytes, timeoutMs) -> Promise<string[]>,
 * sleep(ms), now() }, so a test can replace the board and the clock.
 */
(function (root) {
  'use strict';

  var CMD = { BLE: 0x43, DIAG: 0x45, SCAN: 0x47, SEND: 0x48, JOB: 0x49 };
  var FLAG = { BOUND: 0x01, WIFI: 0x02, CLOUD: 0x08 };

  // A search is a restart, a short scan and a restart; a send adds the connection to the controller, which
  // may need several attempts. The firmware stops trying after 60 s and always finishes within 150 s.
  var WAIT = { search: 120000, send: 170000 };

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

  function errName(e) { return (e && e.improv) || (typeof e === 'string' ? e : e && e.message) || String(e); }

  // ---- reading the controller list -----------------------------------------------------------------------
  // Rows after the third string look like "AA:BB:CC:00:11:22|SF-GGS-1A|-61|3". The name is in the middle and
  // could in principle contain the separator, so address, rssi and flags are taken from the ends.
  function parseStatus(rows) {
    rows = rows || [];
    var count = parseInt(rows[2], 10) || 0;
    var list = [];
    for (var i = 3; i < rows.length && list.length < count; i++) {
      var p = String(rows[i]).split('|');
      if (p.length < 4 || !/^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/.test(p[0])) continue;
      var flags = parseInt(p[p.length - 1], 10);
      list.push({
        addr: p[0],
        name: p.slice(1, p.length - 2).join('|'),
        rssi: parseInt(p[p.length - 2], 10),
        flags: isNaN(flags) || flags < 0 ? null : flags,
      });
    }
    // strongest first: the controller on the desk beats the one in the neighbour's flat
    list.sort(function (a, b) { return (b.rssi || -200) - (a.rssi || -200); });
    return { armed: rows[0] === '1', text: rows[1] || '', controllers: list };
  }

  function describe(c) {
    var bits = [];
    if (!isNaN(c.rssi)) bits.push(c.rssi + ' dBm' + (c.rssi >= -75 ? '' : ', weak'));
    if (c.flags != null) {
      if (c.flags & FLAG.WIFI) bits.push('already on a Wi-Fi network');
      if (c.flags & FLAG.BOUND) bits.push('linked to an account');
    }
    return (c.name || c.addr) + '  (' + bits.join(', ') + ')';
  }

  // ---- waiting for the restarts --------------------------------------------------------------------------
  // Resolves with the diagnostics once the start counter has grown by at least minNew, or null at the
  // deadline. Silence and errors in between are normal: the board is restarting.
  async function waitForBoard(io, boots0, minNew, deadlineMs, onTick) {
    var t0 = io.now();
    while (io.now() - t0 <= deadlineMs) {
      await io.sleep(2500);
      if (onTick) onTick(Math.round((io.now() - t0) / 1000));
      try {
        var d = await io.rpc(CMD.DIAG, [], 3000);
        var boots = parseInt(d && d[1], 10);
        if (!isNaN(boots) && boots >= boots0 + minNew) return d;
      } catch (e) { /* silent or garbled while it restarts */ }
    }
    return null;
  }

  async function bootCounter(io) {
    var d = await io.rpc(CMD.DIAG, [], 4000);
    return parseInt(d && d[1], 10) || 0;
  }

  // ---- search again --------------------------------------------------------------------------------------
  // Returns { ok:true, armed, text, controllers } or { ok:false, code, text }.
  async function searchAgain(io, onTick) {
    var boots0 = await bootCounter(io);
    var ack = await io.rpc(CMD.SCAN, [], 8000);
    if (!ack || ack[0] !== '1') return { ok: false, code: 'BUSY', text: 'The bridge is busy with another Bluetooth job. Wait a minute and try again.' };
    var back = await waitForBoard(io, boots0, 2, WAIT.search, onTick);
    if (!back) return { ok: false, code: 'TIMEOUT', text: 'The bridge did not answer again within two minutes. Press its reset button, wait 20 seconds and try again.' };
    var st = await io.rpc(CMD.BLE, [], 8000);
    var parsed = parseStatus(st);
    parsed.ok = true;
    return parsed;
  }

  // ---- send ----------------------------------------------------------------------------------------------
  // Returns { ok, code, text, state }: text is the bridge's own sentence ("... accepted the Wi-Fi settings ...").
  async function sendToController(io, addr, onTick) {
    var boots0 = await bootCounter(io);
    var ack = await io.rpc(CMD.SEND, enc([addr]), 8000);
    if (!ack || ack[0] !== '1') return { ok: false, code: 'BUSY', text: 'The bridge is busy with another Bluetooth job. Wait a minute and try again.' };

    var back = await waitForBoard(io, boots0, 2, WAIT.send, onTick);
    var job = null;
    try { job = await io.rpc(CMD.JOB, [], 8000); } catch (e) { job = null; }
    if (!job) {
      return { ok: false, code: 'TIMEOUT', text: back ? 'The bridge is back but did not report the result. Open "Logs" on the Control page of its web interface.'
        : 'The bridge did not answer again within three minutes. Press its reset button, wait 20 seconds and try again.' };
    }
    var state = job[0], text = job[2] || '';
    if (state === '2') return { ok: true, code: 'OK', state: state, text: text, addr: job[1] };
    if (state === '1' || state === '0') {
      return { ok: false, code: 'NOT_RUN', state: state, text: 'The bridge restarted but did not run the Bluetooth step. Try again.' };
    }
    return { ok: false, code: 'FAILED', state: state, text: text || 'The controller did not accept the Wi-Fi settings.' };
  }

  var api = {
    CMD: CMD, WAIT: WAIT, enc: enc, errName: errName,
    parseStatus: parseStatus, describe: describe,
    waitForBoard: waitForBoard, searchAgain: searchAgain, sendToController: sendToController,
  };
  root.SBGgs = api;
})(typeof window !== 'undefined' ? window : globalThis);
