// worker.js — runs one Cooja-NG simulation in a Web Worker.
//
// csim-web.js/.wasm is the native simulator (kernel, CPU emulators, radio
// models, the runner and its UI service) compiled with Emscripten.  This file
// only moves data: it fetches the scenario's firmware and config into the
// module's in-memory filesystem, starts the runner with `--ui` exactly as on
// the command line, and relays the UI stream (JSON full states, CBOR deltas)
// and the page's commands.  It simulates nothing itself.
//
// Messages from the page:
//   {type: 'start', scenario: '<key in scenarios.json>', extra: [args...]}
//   {type: 'ws', data: '<JSON command>'}       // what the page would send on a WebSocket
// Messages to the page:
//   {ready: true}  {text: '...'}  {binary: ArrayBuffer}  {log: '...'}
//   {exit: code, tail: '...'}   the runner returned (tail: its last output lines)
//   {error: '...'}              it could not start, or aborted
//
// The page sends commands only after {ready}, so `mod` is always set when
// one arrives; after the run has ended they are dropped.

importScripts('csim-web.js');

let mod = null;
let ended = false;

function toModule(s) {
  const bytes = new TextEncoder().encode(s);
  const p = mod._malloc(bytes.length + 1);
  mod.HEAPU8.set(bytes, p);
  mod._csim_wasm_ui_recv(p, bytes.length);
  mod._free(p);
}

async function fetchBytes(path) {
  const r = await fetch('files/' + path);
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
  return new Uint8Array(await r.arrayBuffer());
}

async function start(key, extra) {
  const scenarios = await (await fetch('scenarios.json')).json();
  if (!Object.hasOwn(scenarios, key)) throw new Error(`unknown scenario '${key}'`);
  const sc = scenarios[key];
  const files = await Promise.all(sc.files.map(async f => [f, await fetchBytes(f)]));

  // The runner's stdout/stderr (packet traces, progress) only reach the page
  // with ?arg=--log; the last lines are always kept for the exit report.
  const verbose = (extra || []).includes('--log');
  const tail = [];
  const out = s => { tail.push(s); if (tail.length > 40) tail.shift(); if (verbose) postMessage({ log: s }); };
  mod = await createCsim({
    print: out,
    printErr: out,
    onExit: code => { ended = true; postMessage({ exit: code, tail: tail.join('\n') }); },
    onAbort: what => { ended = true; postMessage({ error: `simulator aborted: ${what}\n` + tail.join('\n') }); },
  });
  mod.FS.mkdir('/sim');
  for (const [f, bytes] of files) {
    const dir = '/sim/' + f.split('/').slice(0, -1).join('/');
    mod.FS.mkdirTree(dir);
    mod.FS.writeFile('/sim/' + f, bytes);
  }
  mod.FS.chdir('/sim');

  postMessage({ ready: true });
  // Returns at the first yield; the run continues from the event loop, and
  // onExit reports its end.
  mod.callMain(sc.args.concat(['--ui'], (extra || []).filter(a => a !== '--log')));
}

onmessage = (e) => {
  const m = e.data;
  if (m.type === 'start') {
    start(m.scenario, m.extra).catch(err => postMessage({ error: String(err && err.message || err) }));
  } else if (m.type === 'ws' && mod && !ended) {
    toModule(m.data);
  }
};
