// Node test for installer/ggs.js: the "Send Wi-Fi to GGS Controller" logic against a fake board that behaves like the
// real one (restarts into a Bluetooth boot and is silent, counts starts, reports the job outcome), with the byte-exact
// reply formats checked against the firmware's own output (firmware/ggs/host_test -> fixtures.json).
//
//   python firmware/ggs/host_test/run.py && node tests/installer_ggs.test.mjs
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const fixtures = JSON.parse(readFileSync(path.join(here, '../firmware/ggs/host_test/fixtures.json'), 'utf8'));
const sandbox = { console, TextEncoder, TextDecoder, setTimeout, clearTimeout, Promise };
sandbox.globalThis = sandbox;
vm.createContext(sandbox);
vm.runInContext(readFileSync(path.join(here, '../installer/ggs.js'), 'utf8'), sandbox);
const G = sandbox.SBGgs;

let failures = 0;
const check = (ok, what) => { if (!ok) { failures++; console.log('FAIL', what); } else console.log('ok  ', what); };

// ---- the firmware's own bytes -> rows, parsed the way the page parses them ----------------------------------
const hex = (h) => Uint8Array.from(h.match(/../g).map((b) => parseInt(b, 16)));
function rowsOf(h) {   // RPC result frame -> string list
  const f = hex(h); const d = f.slice(9, 9 + f[8]); const out = []; let p = 2;
  while (p < 2 + d[1]) { out.push(new TextDecoder().decode(d.slice(p + 1, p + 1 + d[p]))); p += 1 + d[p]; }
  return out;
}
{
  const st = G.parseStatus(rowsOf(fixtures.ble_found));
  check(st.armed === true && st.controllers.length === 2, 'firmware reply 0x43: armed flag and two controllers parsed');
  check(st.controllers[0].addr === 'AA:BB:CC:00:11:22' && st.controllers[0].rssi === -61 && st.controllers[0].flags === null, 'controller 1: address, rssi, flags unknown');
  check(st.controllers[1].rssi === -78 && st.controllers[1].flags === 3, 'controller 2: flags 3 = on Wi-Fi + bound');
  check(/weak/.test(G.describe(st.controllers[1])) && /already on a Wi-Fi/.test(G.describe(st.controllers[1])) && /linked to an account/.test(G.describe(st.controllers[1])), 'a weak, linked controller is described as such');
  check(!/weak/.test(G.describe(st.controllers[0])), 'a strong controller is not called weak');
  const e = G.parseStatus(rowsOf(fixtures.ble_empty));
  check(e.controllers.length === 0 && e.armed === false, 'firmware reply 0x43 with nothing found');
  const j = rowsOf(fixtures.ble_job_ok);
  check(j.length === 4 && j[0] === '2' && j[1] === 'AA:BB:CC:00:11:22' && /joined/.test(j[2]) && j[3] === '0', 'firmware reply 0x49 (accepted): state, address, text, not pending');
  check(rowsOf(fixtures.ble_job_failed)[0] === '3' && rowsOf(fixtures.ble_job_pending)[0] === '1' && rowsOf(fixtures.ble_job_pending)[3] === '1', 'firmware replies 0x49 (failed, pending)');
  check(rowsOf(fixtures.ble_scan)[0] === '1' && rowsOf(fixtures.ble_send)[0] === '1', 'firmware replies 0x47 / 0x48: started');
}
// rows are sorted strongest first, and a name containing the separator survives
{
  const st = G.parseStatus(['0', '', '3', 'AA:BB:CC:00:00:01|SF-GGS-a|-80|-1', 'AA:BB:CC:00:00:02|SF-GGS|b|-50|2', 'garbage', 'AA:BB:CC:00:00:03|SF-GGS-c|-65|-1']);
  check(st.controllers.map((c) => c.rssi).join() === '-50,-65,-80', 'sorted strongest first');
  check(st.controllers[0].name === 'SF-GGS|b' && st.controllers[0].flags === 2, 'a "|" in the name does not shift rssi and flags');
}

// ---- a fake board with a clock -------------------------------------------------------------------------------
function board(opts) {
  const b = Object.assign({
    boots: 20, now: 0, silentUntil: 0, calls: [], scanned: [], bluetoothResult: null,
    busy: false, sendAccepts: true, hangForever: false, jobState: '0', jobText: '', jobAddr: '', pending: false,
    list: [{ addr: 'AA:BB:CC:00:11:22', name: 'SF-GGS-1A', rssi: -55, flags: 0 }],
    scanBootsAdded: 2, sendBootsAdded: 2, scanSilent: 35000, sendSilent: 70000,
  }, opts || {});
  const io = {
    now: () => b.now,
    sleep: async (ms) => { b.now += ms; },
    rpc: async (cmd, args) => {
      b.calls.push(cmd);
      if (b.now < b.silentUntil || b.hangForever) { const e = new Error('TIMEOUT'); e.improv = 'TIMEOUT'; throw e; }
      if (cmd === G.CMD.DIAG) {
        // the counter grows when the restarts happen, i.e. once the silent period ends
        if (b.restartAt != null && b.now >= b.restartAt) { b.boots += b.restartBoots; b.restartAt = null; }
        return ['Software restart', String(b.boots), '0', '0', '0', '0', '', '100', '90000', '60000', '40000'];
      }
      if (cmd === G.CMD.SCAN) {
        if (b.busy) return ['0'];
        b.silentUntil = b.now + b.scanSilent; b.restartAt = b.silentUntil; b.restartBoots = b.scanBootsAdded; b.scanned = b.list;
        return ['1'];
      }
      if (cmd === G.CMD.SEND) {
        const addr = new TextDecoder().decode(Uint8Array.from(args.slice(1)));
        b.sentTo = addr;
        if (b.busy) return ['0'];
        if (!b.scanned.some((c) => c.addr.toLowerCase() === addr.toLowerCase())) { const e = new Error('INVALID_RPC_PACKET'); e.improv = 'INVALID_RPC_PACKET'; throw e; }
        b.silentUntil = b.now + b.sendSilent; b.restartAt = b.silentUntil; b.restartBoots = b.sendBootsAdded;
        b.jobState = b.sendAccepts ? '2' : '3'; b.jobAddr = addr; b.jobText = b.sendAccepts ? 'SF-GGS-1A accepted the Wi-Fi settings and joined "SpiderBridge" (signal -52 dBm) -- still visible over Bluetooth' : 'Could not connect to ' + addr + ' in time';
        return ['1'];
      }
      if (cmd === G.CMD.BLE) return ['0', '', String(b.scanned.length)].concat(b.scanned.map((c) => c.addr + '|' + c.name + '|' + c.rssi + '|' + (c.flags == null ? -1 : c.flags)));
      if (cmd === G.CMD.JOB) return [b.jobState, b.jobAddr, b.jobText, b.pending ? '1' : '0'];
      throw new Error('UNKNOWN_RPC_COMMAND');
    },
  };
  return { b, io };
}

// 1) search finds controllers and reports them
{
  const { b, io } = board();
  const r = await G.searchAgain(io);
  check(r.ok && r.controllers.length === 1 && r.controllers[0].addr === 'AA:BB:CC:00:11:22', 'search: controller found after the board restarted twice');
  check(b.calls[1] === G.CMD.SCAN, 'search: sends the search command');
}
// 2) search while the bridge is busy
{
  const { io } = board({ busy: true });
  const r = await G.searchAgain(io);
  check(!r.ok && r.code === 'BUSY', 'search: a busy bridge is reported, not waited for');
}
// 3) the board never comes back
{
  const { io, b } = board({ scanSilent: 10 * 60 * 1000 });
  const r = await G.searchAgain(io);
  check(!r.ok && r.code === 'TIMEOUT' && b.now <= G.WAIT.search + 12000, 'search: gives up after the deadline instead of waiting for ever');
}
// 4) send: accepted
{
  const { io, b } = board();
  await G.searchAgain(io);
  const ticks = [];
  const r = await G.sendToController(io, 'AA:BB:CC:00:11:22', (s) => ticks.push(s));
  check(r.ok && r.code === 'OK' && /accepted the Wi-Fi settings/.test(r.text), 'send: accepted, with the bridge\'s own sentence');
  check(b.sentTo === 'AA:BB:CC:00:11:22', 'send: the address reaches the board');
  check(ticks.length > 3 && ticks[ticks.length - 1] >= ticks[0], 'send: progress ticks while waiting');
}
// 5) send: refused by the controller / not in time
{
  const { io } = board({ sendAccepts: false });
  await G.searchAgain(io);
  const r = await G.sendToController(io, 'AA:BB:CC:00:11:22');
  check(!r.ok && r.code === 'FAILED' && /Could not connect/.test(r.text), 'send: the failure text of the bridge is passed on');
}
// 6) send to something the scan did not list: the board refuses, the page reports it
{
  const { io } = board();
  await G.searchAgain(io);
  let err;
  try { await G.sendToController(io, 'AA:BB:CC:99:99:99'); } catch (e) { err = e; }
  check(err && err.improv === 'INVALID_RPC_PACKET', 'send: an address the scan did not list is refused by the board');
}
// 7) the restart happened but the job never ran (pending / none)
{
  const { io, b } = board();
  await G.searchAgain(io);
  const orig = io.rpc;
  io.rpc = async (cmd, a, t) => { const r = await orig(cmd, a, t); if (cmd === G.CMD.JOB) return ['1', 'AA:BB:CC:00:11:22', '', '1']; return r; };
  const r = await G.sendToController(io, 'AA:BB:CC:00:11:22');
  check(!r.ok && r.code === 'NOT_RUN', 'send: a job still marked pending after the restarts is "not run", not success');
}
// 8) bridge busy at send time
{
  const { io, b } = board();
  await G.searchAgain(io); b.busy = true;
  const r = await G.sendToController(io, 'AA:BB:CC:00:11:22');
  check(!r.ok && r.code === 'BUSY', 'send: busy bridge is reported');
}
// 9) board silent for the whole send
{
  const { io } = board({ sendSilent: 10 * 60 * 1000 });
  await G.searchAgain(io);
  const r = await G.sendToController(io, 'AA:BB:CC:00:11:22');
  check(!r.ok && r.code === 'TIMEOUT', 'send: no answer within the deadline -> timeout with advice');
}
// 10) a single restart is not enough (the Bluetooth boot has not yet been followed by the normal boot)
{
  const { io, b } = board({ scanBootsAdded: 1 });
  const r = await G.searchAgain(io);
  check(!r.ok && r.code === 'TIMEOUT', 'search: one restart alone does not count as "back in normal operation"');
}
// 11) request encoding
{
  const a = G.enc(['AA:BB:CC:00:11:22']);
  check(a[0] === 17 && a.length === 18, 'request: address is one length-prefixed string');
}

console.log(failures ? failures + ' FAILURE(S)' : 'ALL ggs tests passed');
process.exit(failures ? 1 : 0);
