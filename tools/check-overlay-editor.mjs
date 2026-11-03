#!/usr/bin/env node
// The web UI's overlay editor (docs/design/overlay-descriptors.md), driven
// over the WebSocket the way the page drives it, against a live
// `test_runner --ui` run with a descriptor in its config:
//
//   - an overlay id is checked before it becomes a panel key: one with a
//     quote (which used to make every "panels" frame invalid JSON, for every
//     page), one over 31 bytes (which never matched its own panel and added
//     one every refresh), and a built-in panel's id are refused, and every
//     panels frame stays valid JSON;
//   - a malformed rule (a map that is a list, a format width that would pad a
//     label to gigabytes) is refused with its line, and the run lives on;
//   - an edited overlay survives a restart and is drawn once: after Remove
//     its panel stays gone (an old instance used to keep drawing it);
//   - a new overlay is refused when the page's panel table is full, rather
//     than reported drawn;
//   - a message over the WebSocket's 16 KB drops that connection only (the
//     page refuses to send one), and the run and other clients go on.
//
// A plain socket client (no WebSocket library, any Node version).
// Usage: node tools/check-overlay-editor.mjs      (RUNNER=... to override)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const RUNNER = path.resolve(process.env.RUNNER || path.join(ROOT, 'build/test_runner'));
let failed = 0;
const check = (name, ok, detail = '') => {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${name}${ok || !detail ? '' : ': ' + detail}`);
  if (!ok) failed++;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* --- a minimal WebSocket client: masked text frames out, frames in --- */
class Ws {
  static open(port) {
    return new Promise((resolve, reject) => {
      const ws = new Ws();
      ws.sock = net.connect(port, '127.0.0.1');
      ws.buf = Buffer.alloc(0);
      ws.frames = [];          // parsed JSON text frames, in order
      ws.bad = [];             // text frames that were not JSON
      ws.closed = false;
      let shook = false;
      ws.sock.on('error', (e) => { ws.closed = true; if (!shook) reject(e); });
      ws.sock.on('close', () => { ws.closed = true; });
      ws.sock.on('data', (d) => {
        ws.buf = Buffer.concat([ws.buf, d]);
        if (!shook) {
          const end = ws.buf.indexOf('\r\n\r\n');
          if (end < 0) return;
          shook = true;
          ws.buf = ws.buf.subarray(end + 4);
          resolve(ws);
        }
        ws.parse();
      });
      ws.sock.write('GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n' +
                    'Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n' +
                    'Sec-WebSocket-Version: 13\r\n\r\n');
    });
  }
  parse() {
    for (;;) {
      if (this.buf.length < 2) return;
      let n = this.buf[1] & 0x7f, off = 2;
      if (n === 126) { if (this.buf.length < 4) return; n = this.buf.readUInt16BE(2); off = 4; }
      else if (n === 127) { if (this.buf.length < 10) return; n = Number(this.buf.readBigUInt64BE(2)); off = 10; }
      if (this.buf.length < off + n) return;
      const op = this.buf[0] & 0x0f, payload = this.buf.subarray(off, off + n);
      this.buf = this.buf.subarray(off + n);
      if (op === 1) {
        const text = payload.toString('utf8');
        try { this.frames.push(JSON.parse(text)); } catch { this.bad.push(text.slice(0, 120)); }
      }
    }
  }
  send(obj) {
    const data = Buffer.from(JSON.stringify(obj), 'utf8'), mask = crypto.randomBytes(4);
    let head;
    if (data.length < 126) head = Buffer.from([0x81, 0x80 | data.length]);
    else if (data.length < 65536) { head = Buffer.from([0x81, 0x80 | 126, 0, 0]); head.writeUInt16BE(data.length, 2); }
    else { head = Buffer.alloc(10); head[0] = 0x81; head[1] = 0x80 | 127; head.writeBigUInt64BE(BigInt(data.length), 2); }
    const body = Buffer.from(data.map((b, i) => b ^ mask[i & 3]));
    this.sock.write(Buffer.concat([head, mask, body]));
  }
  // The "overlays" answer to our token (waits up to `ms`).
  async ask(cmd, extra = {}, ms = 4000) {
    const token = crypto.randomBytes(6).toString('hex');
    this.send({ cmd: 'overlay-' + cmd, token, ...extra });
    for (const t0 = Date.now(); Date.now() - t0 < ms; await sleep(50)) {
      const a = this.frames.find((f) => f.type === 'overlays' && f.token === token);
      if (a) return a;
    }
    return null;
  }
  // The panels frames that arrive in the next `ms`.
  async panels(ms) {
    const from = this.frames.length;
    await sleep(ms);
    return this.frames.slice(from).filter((f) => f.type === 'panels').map((f) => f.panels);
  }
  close() { this.sock.destroy(); }
}

const descriptor = (id, title, extra = '') =>
  `overlay: ${id}\ntitle: "${title}"\nsymbol: no_such_variable\nstruct: S\nfields: { a: b }\n` +
  `node:\n  label: "{a}"\n${extra}`;

/* --- the run: a Sky node, one descriptor from its config --- */
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'csim-oved-'));
fs.writeFileSync(path.join(dir, 'base.yaml'), descriptor('ed-base', 'from the config'));
fs.writeFileSync(path.join(dir, 'run.yaml'),
  'version: 2\ntimeout_ms: 600000\noverlays: [base.yaml]\n' +
  `mote_types:\n  - { name: sky, firmware: ${path.join(ROOT, 'firmware/sky/hello-world.sky')} }\n` +
  'nodes:\n  - { type: sky, id: 1, x: 0, y: 0 }\n');
const port = await new Promise((r) => { const s = net.createServer().listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); });
const proc = spawn(RUNNER, ['test', path.join(dir, 'run.yaml'), '--ui', String(port), '-q'],
                   { stdio: ['ignore', 'ignore', 'pipe'] });
let stderr = '';
proc.stderr.on('data', (d) => { stderr += d; });
let ws;
for (const t0 = Date.now(); !ws && Date.now() - t0 < 10000; await sleep(100)) {
  if (proc.exitCode !== null) break;
  try { ws = await Ws.open(port); } catch { /* not listening yet */ }
}
if (!ws) { console.log(`check-overlay-editor: FAIL: the UI never started (${stderr.trim()})`); process.exit(1); }
ws.send({ cmd: 'full' });
ws.send({ cmd: 'play' });

try {
  console.log('== ids: the panel key');
  let p = await ws.panels(1500);
  check('the config\'s descriptor is drawn', p.some((x) => x['ed-base']));
  for (const [what, id] of [['a quote', `'q":{"title":"FORGED'`], ['32 bytes', 'openthread-router-table-descript'],
                            ['34 bytes', 'openthread-router-table-descriptor'], ['a built-in panel\'s', 'thread']]) {
    const a = await ws.ask('apply', { yaml: descriptor(id, 'x') });
    check(`an id with ${what} is refused`, a && a.error && !a.applied, JSON.stringify(a));
  }
  p = await ws.panels(1500);
  check('every panels frame is JSON', ws.bad.length === 0 && p.length > 0, ws.bad[0]);
  check('no panel added for a refused id', p.every((x) => Object.keys(x).length === 1), JSON.stringify(p.at(-1)));

  console.log('== rules: checked before they are drawn');
  let a = await ws.ask('apply', { yaml: descriptor('ed-x', 'x', '  fill: { field: a, map: [red] }\n') });
  check('a map that is a list is refused, with its line', a && /:8: fill: 'map'/.test(a.error || ''), JSON.stringify(a));
  a = await ws.ask('apply', { yaml: descriptor('ed-x', 'x').replace('"{a}"', '"{a:2147483646x}"') });
  check('a format width of 10 digits is refused', a && /width of up to 2 digits/.test(a.error || ''), JSON.stringify(a));
  await sleep(600);
  check('the run lives on', proc.exitCode === null, stderr.slice(-300));

  console.log('== an edited overlay across a restart');
  a = await ws.ask('apply', { yaml: descriptor('ed-base', 'edited') });
  check('the edit is applied', a && a.applied === 'ed-base', JSON.stringify(a));
  ws.send({ cmd: 'restart' });
  await sleep(2500);
  ws.send({ cmd: 'play' });
  a = await ws.ask('list');
  check('after the restart the editor lists it, edited', a && a.list.length === 1 && /edited/.test(a.list[0].yaml), JSON.stringify(a && a.list));
  p = await ws.panels(1500);
  check('and it is drawn, edited', p.length > 0 && p.every((x) => x['ed-base'] && x['ed-base'].title === 'edited'),
        JSON.stringify(p.at(-1)));
  a = await ws.ask('remove', { id: 'ed-base' });
  check('Remove takes it off', a && !a.error && a.list.length === 0, JSON.stringify(a));
  p = await ws.panels(2500);
  check('and its panel stays gone (no old instance draws it)', p.length > 0 && p.every((x) => !x['ed-base']),
        JSON.stringify(p.find((x) => x['ed-base'])));

  console.log('== the panel table');
  const got = [];
  for (let i = 1; i <= 9; i++) got.push(await ws.ask('apply', { yaml: descriptor('ed-' + i, 'p' + i) }));
  check('eight overlays fit', got.slice(0, 8).every((x) => x && x.applied), JSON.stringify(got.slice(0, 8).map((x) => x && (x.applied || x.error))));
  check('the ninth is refused, not reported drawn', got[8] && !got[8].applied && /no room/.test(got[8].error || ''), JSON.stringify(got[8]));

  console.log('== a message over 16 KB');
  const other = await Ws.open(port);
  const big = descriptor('ed-big', 'big', '# ' + 'x'.repeat(17000) + '\n');
  ws.send({ cmd: 'overlay-apply', token: 't', yaml: big });
  await sleep(800);
  check('drops the connection that sent it', ws.closed);
  other.send({ cmd: 'full' });
  p = await other.panels(1500);
  check('the run and the other client go on', proc.exitCode === null && p.length > 0);
  other.close();
} finally {
  if (ws) ws.close();
  proc.kill('SIGKILL');
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log(failed ? `check-overlay-editor: ${failed} FAILED` : 'check-overlay-editor: all passed');
process.exit(failed ? 1 : 0);
