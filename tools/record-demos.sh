#!/usr/bin/env bash
#
# Record the web-UI replays the GitHub Pages site plays (site/, docs/ui-replay.md).
#
#   tools/record-demos.sh OUT_DIR            # all demos
#   tools/record-demos.sh OUT_DIR mixed tz   # some of them (the others stay in the manifest)
#
# Each demo is an ordinary run of build/test_runner with --ui-record: headless,
# unpaced, the stream a live browser would receive.  The site only shows runs
# that did what their card says, so a demo is published only if
#   - the run passes its own test (the configs that have validators),
#   - the recording parses as a replay document, and
#   - the recording shows the demo's expectation: a console pattern seen on at
#     least N distinct nodes (the third field of DEMOS) — this is what gates
#     the configs that have no validators of their own.
# A recording is written beside its final name and moved into place only once
# it has passed, so a failed run never leaves a stale or partial file behind.
# OUT_DIR gets one <name>.json per demo plus demos.json, the manifest the
# landing page reads (title, file, size, simulated seconds, nodes, frames, the
# command that made it).
#
# The demo list is here and nowhere else: add a line to DEMOS and a card to
# site/index.html with the same name (the Pages build checks every card has a
# recording).
set -euo pipefail

usage="usage: tools/record-demos.sh OUT_DIR [name...]"
OUT=${1:?$usage}
shift
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)              # relative to the caller, before the cd below
cd "$(dirname "$0")/.."
BIN=${BIN:-build/test_runner}
[ -x "$BIN" ] || { echo "record-demos: $BIN not built (run make)" >&2; exit 2; }
WALL=${WALL:-600}                    # seconds of wall time any one demo may take

# name|test_runner arguments|console pattern>=distinct nodes
DEMOS=(
    "mixed|test configs/test-mixed-platform-rpl.yaml|Received response>=2"
    "grid100|test configs/udgm-100node-grid-sky.json -t 60000|Received response>=10"
    "grid16|test configs/ui-rpl-udp-grid.json -t 60000|Received response>=12"
    "trustzone|test configs/test-tz-rpl-udp-nrf54l15-xiao.yaml|Received response>=1"
    "tsch|test configs/test-tsch-nrf52840-dk.json|"
    "energest|test configs/plugin-energest-builtin-v2.json|Received response>=1"
    "zephyr|test configs/interop-contiki-zephyr.json|Received and replied>=1"
    "riot|test configs/interop-contiki-riot.json|RIOT says>=1"
    "openthread|test configs/test-openthread-cc2538dk.yaml|child>=2"
)

known() { local spec; for spec in "${DEMOS[@]}"; do [ "${spec%%|*}" = "$1" ] && return 0; done; return 1; }
for s in "$@"; do
    known "$s" || { echo "record-demos: no demo named '$s'" >&2; exit 2; }
done
is_selected() {
    [ $# -gt 0 ] || return 1
    local want=$1; shift
    [ $# -eq 0 ] && return 0
    local s; for s in "$@"; do [ "$s" = "$want" ] && return 0; done
    return 1
}

done_specs=()
for spec in "${DEMOS[@]}"; do
    IFS='|' read -r name argstr expect <<< "$spec"
    is_selected "$name" "$@" || continue
    read -r -a args <<< "$argstr"
    tmp="$OUT/$name.json.part"
    rm -f "$tmp"
    echo "record-demos: $name: $BIN ${args[*]} -q --ui-record $name.json"
    if ! "$BIN" "${args[@]}" -q --wall-timeout "$WALL" --ui-record "$tmp" \
            > "$OUT/$name.log" 2>&1; then
        echo "record-demos: $name: the run failed; its log:" >&2
        tail -30 "$OUT/$name.log" >&2
        rm -f "$tmp"
        exit 1
    fi
    done_specs+=("$name|${args[*]}|$expect")
done

python3 - "$OUT" "${done_specs[@]}" <<'EOF'
import json, os, re, sys
out, specs = sys.argv[1], sys.argv[2:]

def check(name, path, expect):
    with open(path, encoding='utf-8') as f:
        doc = json.load(f)                     # a broken recording stops the build here
    if doc.get('format') != 'cooja-ng-ui-replay/1' or not doc.get('full') or not doc.get('deltas'):
        sys.exit(f'record-demos: {name}: not a usable replay document')
    if expect:
        m = re.fullmatch(r'(.+)>=(\d+)', expect)
        pattern, need = m.group(1), int(m.group(2))
        lines = [(str(n['id']), l) for n in doc['full']['nodes'] for l in n.get('console', [])]
        lines += [(k, l) for d in doc['deltas'] for k, v in d.get('5', {}).items() for l in v]
        seen = {k for k, l in lines if pattern in l}
        if len(seen) < need:
            sys.exit(f'record-demos: {name}: "{pattern}" on {len(seen)} node(s), '
                     f'the demo needs {need}: not published')
    return doc

# A partial re-record keeps the manifest entries of the demos it skipped.
mpath = os.path.join(out, 'demos.json')
try:
    with open(mpath) as f:
        old = {d['name']: d for d in json.load(f).get('demos', [])}
except (OSError, ValueError):
    old = {}

entries = {}
for spec in specs:
    name, args, expect = spec.split('|', 2)
    part = os.path.join(out, name + '.json.part')
    try:
        doc = check(name, part, expect)
    except SystemExit:
        os.remove(part)
        raise
    os.replace(part, os.path.join(out, name + '.json'))
    path = os.path.join(out, name + '.json')
    deltas, nodes = doc['deltas'], doc['full']['nodes']
    entries[name] = {
        'name': name,
        'file': name + '.json',
        'title': doc['run'].get('title', ''),
        'scenario': doc['run'].get('scenario', ''),
        'command': 'test_runner ' + args + ' -q --ui-record ' + name + '.json',
        'bytes': os.path.getsize(path),
        'sim_s': round(deltas[-1]['0'] / 1000),
        'nodes': len(nodes),
        'cpus': sorted({n['type'] for n in nodes}),
        'frames': sum(1 for d in deltas for e in d.get('6', []) if e[2] == 7),
        'deltas': len(deltas),
    }
    os.remove(os.path.join(out, name + '.log'))

order = [s.split('|', 1)[0] for s in specs]
for name, d in old.items():
    if name not in entries and os.path.exists(os.path.join(out, d.get('file', ''))):
        entries[name] = d
        order.append(name)
with open(mpath, 'w') as f:
    json.dump({'format': 'cooja-ng-demos/1', 'demos': [entries[n] for n in order]}, f, indent=1)
for n in order:
    m = entries[n]
    print(f"record-demos: {m['name']:10} {m['sim_s']:4d} s  {m['nodes']:3d} nodes  "
          f"{m['frames']:5d} frames  {m['bytes'] // 1024:5d} KB"
          + ('' if n in [s.split('|', 1)[0] for s in specs] else '  (kept)'))
EOF
