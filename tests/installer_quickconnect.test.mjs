// Node test for installer/quickconnect.js (protocol + password logic) against the byte-exact
// replies produced by the real firmware code (firmware/ggs/host_test -> fixtures.json).
//
//   python firmware/ggs/host_test/run.py && node tests/installer_quickconnect.test.mjs
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const fixtures = JSON.parse(readFileSync(path.join(here, '../firmware/ggs/host_test/fixtures.json'), 'utf8'));
const hex = (h) => Uint8Array.from(h.match(/../g).map((b) => parseInt(b, 16)));

const sandbox = { window: {}, document: { createElement() {}, body: {} }, console, crypto: globalThis.crypto, TextEncoder, TextDecoder, setTimeout, clearTimeout, Promise, Uint8Array, Uint32Array, URL };
sandbox.window = sandbox;
vm.createContext(sandbox);
vm.runInContext(readFileSync(path.join(here, '../installer/quickconnect.js'), 'utf8'), sandbox);
const { ImprovLink, randomHotspotPassword, enc, CMD } = sandbox.SBQuickConnect._test;

let failures = 0;
const check = (ok, what) => { if (!ok) { failures++; console.log('FAIL', what); } else console.log('ok  ', what); };

// A fake serial port: whatever the client writes is answered with the fixture frames, so the
// client's real framing, checksum and string parsing run against firmware output.
function link(replyHex, onWrite) {
  const l = new ImprovLink({ readable: { getReader() { return { read: () => new Promise(() => {}), releaseLock() {}, cancel: async () => {} }; } }, writable: { getWriter() { return { write: async (b) => { onWrite && onWrite(b); setTimeout(() => l._feed(hex(replyHex)), 5); }, releaseLock() {} }; } } });
  l.writer = l.port.writable.getWriter();
  return l;
}

// 1) password generator: same rules as the firmware
{
  let ok = true, set = new Set();
  for (let i = 0; i < 500; i++) {
    const p = randomHotspotPassword();
    set.add(p);
    if (p.length !== 15 || !/[a-z]/.test(p) || !/[A-Z]/.test(p) || !/[0-9]/.test(p) || !/[#!]/.test(p) || /[lIO01]/.test(p)) ok = false;
  }
  check(ok, 'random hotspot password: 15 chars, all classes, no look-alikes (500 samples)');
  check(set.size === 500, 'random hotspot password: 500 distinct values');
}

// 2) request encoding
{
  const a = enc(['HomeNet', 'pw12345678', 'HotspotPw#789', '1']);
  check(a[0] === 7 && a.length === 1 + 7 + 1 + 10 + 1 + 13 + 1 + 1, 'enc(): length-prefixed strings');
  let threw = false; try { enc(['x'.repeat(256)]); } catch (e) { threw = true; }
  check(threw, 'enc(): rejects a string over 255 bytes');
}

// 3) request frame sent for 0x42 has the right header and checksum
{
  let sent;
  const l = link(fixtures.wifi_ap_quick, (b) => { sent = Array.from(b); });
  await l.call(CMD.WIFI_AP, enc(['HomeNet', 'wifipass123', 'HotspotPw#789', '1']), 1000);
  const sum = sent.slice(0, -2).reduce((s, x) => s + x, 0) & 255;
  check(String.fromCharCode(...sent.slice(0, 6)) === 'IMPROV' && sent[6] === 1 && sent[7] === 3 && sent[9] === 0x42, 'request: IMPROV header, RPC, command 0x42');
  check(sent[sent.length - 2] === sum && sent[sent.length - 1] === 10, 'request: checksum and newline');
}

// 4) real firmware replies are parsed
{
  let r = await link(fixtures.wifi_ap_quick).call(CMD.WIFI_AP, [], 1000);
  check(r.length === 3 && r[1] === 'SpiderBridge' && r[0] === 'http://192.168.1.77/', 'wifi_ap_quick: 3 strings returned (url, hotspot SSID, password)');
  check(r[0] === 'http://192.168.1.77/' && r[2] === 'HotspotPw#789', 'wifi_ap_quick: url and the hotspot password the firmware stored');

  r = await link(fixtures.wifi_ap_random).call(CMD.WIFI_AP, [], 1000);
  check(r[2] === 'RandomPw#2345!x', 'wifi_ap_random: returns the password the firmware generated');

  r = await link(fixtures.netinfo).call(CMD.NETINFO, [], 1000);
  check(r.length === 10 && r[0] === 'HomeNet' && r[1] === '192.168.1.77' && r[4] === '192.168.10.1' && r[9] === '0.0.0+test', 'netinfo: 10 strings');

  r = await link(fixtures.ble_found).call(CMD.BLE, [], 1000);
  check(r[0] === '1' && r[2] === '2' && r[3] === 'AA:BB:CC:00:11:22|SF-GGS-1A|-61', 'ble_found: armed flag and controller list');
  r = await link(fixtures.ble_empty).call(CMD.BLE, [], 1000);
  check(r[0] === '0' && r[2] === '0' && r.length === 3, 'ble_empty: not armed, none found');
}

// 5) failures surface as the Improv error name
{
  let err;
  try { await link(fixtures.wifi_ap_fail).call(CMD.WIFI_AP, [], 1000); } catch (e) { err = e; }
  check(err && err.improv === 'UNABLE_TO_CONNECT', 'wifi_ap_fail: UNABLE_TO_CONNECT');
}

// 6) log lines mixed into the stream do not break parsing, packets split over chunks do not either
{
  const l = link(fixtures.netinfo);
  const noise = new TextEncoder().encode('I (123) wifi: state: run\r\nIMP junk IMPRO\r\n');
  const p = l.call(CMD.NETINFO, [], 1000);
  l._feed(noise);
  const frame = hex(fixtures.netinfo);
  l._feed(frame.slice(0, 5)); l._feed(frame.slice(5, 40)); l._feed(frame.slice(40));
  const r = await p;
  check(r.length === 10 && r[0] === 'HomeNet', 'parser survives log text and split chunks');
}

// 7) a corrupted checksum is ignored (times out) instead of being parsed
{
  const bad = hex(fixtures.netinfo); bad[20] ^= 0xFF;
  const l = new ImprovLink({ readable: {}, writable: {} });
  l.writer = { write: async () => {} };
  const p = l.call(CMD.NETINFO, [], 150);
  l._feed(bad);
  let err; try { await p; } catch (e) { err = e; }
  check(err && err.improv === 'TIMEOUT', 'corrupted checksum is ignored');
}

console.log(failures ? failures + ' FAILURE(S)' : 'ALL quickconnect tests passed');
process.exit(failures ? 1 : 0);
