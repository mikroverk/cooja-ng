#!/usr/bin/env node
// The web UI's layer merging, run in Node on the functions ui/index.html
// ships (no browser, no library): activeLayers() takes each node's style
// from the first visible layer that styles it, every visible layer's links,
// and every visible layer's node commands; actionItemsHtml() lists the
// commands on the node menu, one with an empty command disabled.
//
// Usage: node tools/check-ui-layers.mjs [ui/index.html]
import { readFileSync } from 'node:fs';

const page = readFileSync(process.argv[2] || 'ui/index.html', 'utf8');

// The source of `function NAME(...) { ... }`, braces matched (the bodies
// hold no braces in strings or comments that would unbalance them).
function fn(name) {
  const at = page.indexOf('function ' + name + '(');
  if (at < 0) throw new Error('ui/index.html has no function ' + name);
  let i = page.indexOf('{', at), depth = 0;
  for (; i < page.length; i++) {
    if (page[i] === '{') depth++;
    else if (page[i] === '}' && --depth === 0) return page.slice(at, i + 1);
  }
  throw new Error('unbalanced function ' + name);
}

let pluginLayers = {}, layersHidden = new Set();
const api = new Function('getLayers', 'getHidden',
  'let pluginLayers, layersHidden;\n' +
  fn('esc') + '\n' + fn('activeLayers') + '\n' + fn('actionItemsHtml') + '\n' +
  'return { activeLayers() { pluginLayers = getLayers(); layersHidden = getHidden(); return activeLayers(); },' +
  '         actionItemsHtml };')(() => pluginLayers, () => layersHidden);

let checks = 0, failed = 0;
function check(ok, what) {
  checks++;
  if (!ok) { failed++; console.log('  FAIL: ' + what); }
}

const lead = ['Become leader of a new partition (leaderweight 65, state leader)', 'leaderweight 65\nstate leader'];
const det = ['Detach and reattach (state detached)', 'state detached'];
pluginLayers = {
  rpl:    { nodes: { 1: { glyph: 'square', label: 'rank 128' }, 2: { glyph: 'circle' } },
            links: [{ from: 2, to: 1, style: 'parent' }] },
  thread: { nodes: { 2: { glyph: 'hex', actions: [lead, det, ['bad']] }, 3: { glyph: 'circle', actions: [det] } },
            links: [{ from: 3, to: 2, style: 'parent' }] },
};
let L = api.activeLayers();
check(L.nodes[2].glyph === 'circle', 'the first visible layer (rpl) styles node 2');
check(L.links.length === 2, 'both layers\' links are drawn');
check(JSON.stringify(L.actions[2]) === JSON.stringify([lead, det]),
      'node 2 keeps the thread layer\'s commands under rpl\'s style (a malformed one dropped): ' + JSON.stringify(L.actions[2]));
check(JSON.stringify(L.actions[3]) === JSON.stringify([det]), 'node 3, styled by thread alone, has its commands');
check(!L.actions[1], 'node 1 has none');

layersHidden = new Set(['thread']);
L = api.activeLayers();
check(!L.actions[2] && L.links.length === 1, 'a hidden layer adds no commands and no links');

layersHidden = new Set();
pluginLayers.other = { nodes: { 2: { actions: [['Ping', 'ping ff03::1']] } } };
L = api.activeLayers();
check(L.actions[2].length === 3 && L.actions[2][2][1] === 'ping ff03::1', 'commands of two layers are both listed, in layer order');

const html = api.actionItemsHtml([lead, ['Become leader: no (weight 255)', '']]);
check(html.startsWith('<div class="sep"></div>'), 'the commands follow a separator');
check(html.includes('<div data-act="0">Become leader of a new partition'), 'a command is a clickable item');
check(html.includes('<div class="off">Become leader: no (weight 255)</div>') && !html.includes('data-act="1"'),
      'an empty command is shown disabled, with nothing to send');
check(api.actionItemsHtml([]) === '', 'no commands, no separator');
check(api.actionItemsHtml([['<b>', 'x']]).includes('&lt;b&gt;'), 'labels are escaped');

console.log(`check-ui-layers: ${checks - failed} of ${checks} checks passed`);
process.exit(failed ? 1 : 0);
