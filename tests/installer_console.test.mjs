// Node test for the byte-level packet filter of installer/console.js: Improv packets (binary) must never
// show up in the log text, however the stream is cut into reads, and the log text around them must survive.
//
//   python firmware/ggs/host_test/run.py && node tests/installer_console.test.mjs
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const fixtures = JSON.parse(readFileSync(path.join(here, '../firmware/ggs/host_test/fixtures.json'), 'utf8'));
const hex = (h) => Array.from(h.match(/../g).map((b) => parseInt(b, 16)));
const enc = (s) => Array.from(new TextEncoder().encode(s));

const noopEl = () => ({ appendChild() {}, addEventListener() {}, setAttribute() {}, style: {}, set textContent(v) {}, set className(v) {}, set type(v) {}, set disabled(v) {} });
const sandbox = {
  window: {}, console, TextEncoder, TextDecoder, setTimeout, clearTimeout, Uint8Array,
  navigator: {},          // no Web Serial: the console must still mount and filter text
  document: { createElement: noopEl, body: noopEl() },
};
sandbox.window = sandbox;
vm.createContext(sandbox);
vm.runInContext(readFileSync(path.join(here, '../installer/console.js'), 'utf8'), sandbox);

const root = noopEl();
const con = sandbox.SBConsole.mount(root);

let failures = 0;
const check = (ok, what) => { if (!ok) { failures++; console.log('FAIL', what); } else console.log('ok  ', what); };

// run a list of reads through the filter and the decoder exactly like the read loop does
function feed(reads) {
  con.carry = null;
  const dec = new TextDecoder('utf-8', { fatal: false });
  let text = '';
  for (const r of reads) text += dec.decode(con.stripPackets(Uint8Array.from(r)), { stream: true });
  return text;
}

const pkt = hex(fixtures.diag);
const line = (s) => enc(s + '\r\n');

check(feed([line('I (100) a: hello')]) === 'I (100) a: hello\r\n', 'plain log text is untouched');
check(feed([[...line('before'), ...pkt, ...line('after')]]) === 'before\r\nafter\r\n', 'a packet in the middle of the text is removed, the text around it stays');
check(feed([pkt]) === '', 'a packet alone leaves nothing');

// every possible cut of the stream into two reads
{
  const all = [...line('one'), ...pkt, ...line('two')];
  let bad = 0;
  for (let cut = 1; cut < all.length; cut++) if (feed([all.slice(0, cut), all.slice(cut)]) !== 'one\r\ntwo\r\n') bad++;
  check(bad === 0, 'a packet cut into two reads at every possible position (' + (all.length - 1) + ' cuts)');
}
// one byte at a time
{
  const all = [...line('x'), ...pkt, ...line('y'), ...hex(fixtures.netinfo), ...line('z')];
  check(feed(all.map((b) => [b])) === 'x\r\ny\r\nz\r\n', 'a byte at a time, two packets between three lines');
}
// Text that looks like the start of a marker is not text the filter can recognise as log: "IMPROV" followed by
// a length byte is always treated as a packet (the firmware never prints that word). What must hold is that it
// cannot make the filter wait for ever or swallow the lines after it.
{
  const out = feed([enc('IMPRO'), enc('xx normal text\r\n'), line('next line')]);
  check(out.includes('next line') && out.includes('normal text'), 'a partial marker followed by other text does not swallow what follows');
  con.carry = null;
  const lone = feed([enc('IMPRO')]);
  check(lone === '' && con.carry && con.carry.length === 5, 'a marker cut off at the end of a read is held back until the next read');
}
// multi-byte UTF-8 across the cut
{
  const all = enc('temp 21\u00b0C ok\r\n');
  const cut = all.indexOf(0xC2) + 1;      // between the two bytes of the degree sign
  check(feed([all.slice(0, cut), all.slice(cut)]) === 'temp 21\u00b0C ok\r\n', 'a multi-byte character split across reads is decoded whole');
}
// 400 random cuts of a long mixed stream
{
  const all = [];
  for (let i = 0; i < 30; i++) { all.push(...line('log line ' + i)); if (i % 4 === 0) all.push(...pkt); }
  const want = Array.from({ length: 30 }, (_, i) => 'log line ' + i + '\r\n').join('');
  let bad = 0;
  for (let n = 0; n < 400; n++) {
    const reads = []; let p = 0;
    while (p < all.length) { const l = 1 + Math.floor(Math.random() * 40); reads.push(all.slice(p, p + l)); p += l; }
    if (feed(reads) !== want) bad++;
  }
  check(bad === 0, 'a long mixed stream cut at random, 400 times');
}

console.log(failures ? failures + ' FAILURE(S)' : 'ALL console tests passed');
process.exit(failures ? 1 : 0);
