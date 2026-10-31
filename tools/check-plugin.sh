#!/bin/bash
#
# Phase 9 plugin smoke check.
#
# Builds the example packet-sink plugin and asserts:
#   1. loading it via --plugin prints a non-zero observer tally at teardown;
#   2. the tally is deterministic across two runs;
#   3. a missing .so or an unknown built-in name is refused: exit 2, with
#      the reason on stderr, before the run starts;
#   4. the rpl overlay draws the DODAG it should — on a Sky chain from
#      firmware memory and from the packets alone, on Sky + CC2538 +
#      nRF52840 from two struct layouts — deterministically, and without
#      changing the run (the same PCAP with and without it).
#
# Usage: tools/check-plugin.sh
#
set -u
RUNNER=${RUNNER:-./build/test_runner}
SO=build/plugins/packet_sink.so
CFG=${CFG:-configs/test-rpl-udp-sky.json}
T=${T:-30000}

# GNU make: the Makefile is GNU syntax, and "make" is BSD make on the BSDs.
case $(uname -s) in *BSD|DragonFly) MAKE=${MAKE:-gmake} ;; *) MAKE=${MAKE:-make} ;; esac
$MAKE plugins >/dev/null 2>&1 || { echo "FAIL: build plugins"; exit 1; }
[ -f "$SO" ] || { echo "FAIL: $SO not built"; exit 1; }

t1=$("$RUNNER" test "$CFG" -t "$T" --plugin "$SO" 2>&1 | grep -E '^packet-sink:')
t2=$("$RUNNER" test "$CFG" -t "$T" --plugin "$SO" 2>&1 | grep -E '^packet-sink:')

if [ -z "$t1" ]; then echo "FAIL: no packet-sink tally line"; exit 1; fi
if [ "$t1" != "$t2" ]; then echo "FAIL: tally not deterministic"; echo " $t1"; echo " $t2"; exit 1; fi
# Assert the tally observed *something* (log lines or uart bytes are always emitted).
if echo "$t1" | grep -qE '0 log lines, 0 uart bytes'; then echo "FAIL: tally is empty: $t1"; exit 1; fi

# Missing .so: refused with the reason, exit 2 (not a run without it).
miss=$("$RUNNER" test "$CFG" -t 2000 --plugin /no/such/plugin.so -q 2>&1)
rc=$?
if [ "$rc" -ne 2 ]; then echo "FAIL: missing .so gave rc=$rc, want 2"; exit 1; fi
echo "$miss" | grep -q '^plugin: /no/such/plugin.so: ' || { echo "FAIL: missing .so not reported"; exit 1; }
# A misspelt built-in name in a config: the same.
tmpcfg=$(mktemp "${TMPDIR:-/tmp}/csim-plugin-XXXXXX") && mv "$tmpcfg" "$tmpcfg.json" && tmpcfg="$tmpcfg.json"
sed 's/"energest"/"energist"/' configs/plugin-energest-builtin-v2.json > "$tmpcfg"
grep -q '"energist"' "$tmpcfg" || { echo "FAIL: could not make the misspelt config"; rm -f "$tmpcfg"; exit 1; }
miss=$("$RUNNER" test "$tmpcfg" -t 2000 -q 2>&1)
rc=$?
rm -f "$tmpcfg"
if [ "$rc" -ne 2 ]; then echo "FAIL: unknown built-in plugin gave rc=$rc, want 2"; exit 1; fi
echo "$miss" | grep -q "unknown builtin service 'energist'" || { echo "FAIL: unknown built-in not reported"; exit 1; }

echo "PLUGIN OK: $t1"
echo "PLUGIN OK: a missing .so and an unknown built-in are refused (exit 2)"

# --- Radio-medium plugin (Phase 11): the lossy medium selected by config v2 ---
MED_SO=build/plugins/lossy_medium.so
MED_CFG=configs/medium-plugin-sky-v2.json
[ -f "$MED_SO" ] || { echo "FAIL: $MED_SO not built"; exit 1; }
m1=$("$RUNNER" test "$MED_CFG" -t 30000 2>&1 | grep -E '^Radio medium:|^  Total RF bytes:')
m2=$("$RUNNER" test "$MED_CFG" -t 30000 2>&1 | grep -E '^Radio medium:|^  Total RF bytes:')
if ! echo "$m1" | grep -q 'Radio medium: lossy (plugin)'; then
    echo "FAIL: lossy medium not selected"; echo " $m1"; exit 1; fi
if [ "$m1" != "$m2" ]; then echo "FAIL: medium plugin not deterministic"; exit 1; fi
echo "PLUGIN OK: medium plugin '$(echo "$m1" | head -1)' (deterministic)"

# --- Energy plugin (energest): a COMPILED-IN plugin selected by config name ---
# (no .so — config plugins:["energest"] resolves to the built-in service).
EN_CFG=configs/plugin-energest-builtin-v2.json
e1=$("$RUNNER" test "$EN_CFG" -t "$T" 2>&1 | grep -E '^energest: network total')
e2=$("$RUNNER" test "$EN_CFG" -t "$T" 2>&1 | grep -E '^energest: network total')
if [ -z "$e1" ]; then echo "FAIL: no energest total line (built-in not attached?)"; exit 1; fi
if [ "$e1" != "$e2" ]; then echo "FAIL: energest not deterministic"; echo " $e1"; echo " $e2"; exit 1; fi
# A radio that was tracked at all charges some energy.
if echo "$e1" | grep -qE '~0\.000 mJ'; then echo "FAIL: energest charged nothing: $e1"; exit 1; fi
# The per-mote line carries the CPU/LPM axis (SIM_OBS_CPU_STATE snapshot).
em=$("$RUNNER" test "$EN_CFG" -t "$T" 2>&1 | grep -E '^energest:  mote ' | head -1)
if ! echo "$em" | grep -qE 'cpu [0-9].* lpm [0-9]'; then
    echo "FAIL: energest missing cpu/lpm axis: $em"; exit 1; fi
echo "PLUGIN OK: energest built-in by name '$e1' (radio+cpu/lpm, deterministic)"

# --- The rpl overlay (a canvas layer from firmware variables / packets) ---
# CSIM_DUMP_PANELS prints the published panels at the end of the run; the
# checker below asserts the DODAG in the "rpl" (or "rpl-packets") layer:
# root, one parent link per other node (to the expected parent), one colour.
panels() { CSIM_DUMP_PANELS=1 "$RUNNER" test "$@" -q 2>&1 | sed -n 's/^UI panels: //p'; }
dodag() {  # dodag ID "child:parent ..." JSON
    python3 -c '
import json, sys
pid, want, doc = sys.argv[1], dict(p.split(":") for p in sys.argv[2].split()), sys.argv[3]
L = json.loads(doc)[pid]["layer"]
nodes, links = L["nodes"], {str(l["from"]): str(l["to"]) for l in L["links"] if l["style"] == "parent"}
assert nodes["1"]["glyph"] == "square", "node 1 is not drawn as the root: %r" % nodes.get("1")
assert links == want, "parent links %r, want %r" % (links, want)
fills = {n.get("fill") for n in nodes.values()}
assert len(fills) == 1 and None not in fills, "not one DODAG colour: %r" % fills
print(json.loads(doc)[pid].get("text", "").splitlines()[0])
' "$1" "$2" "$3"
}
CHAIN=configs/test-rpl-overlay-chain-sky.yaml
o1=$(panels "$CHAIN")
o2=$(panels "$CHAIN")
[ -n "$o1" ] || { echo "FAIL: the rpl overlay published no panel"; exit 1; }
[ "$o1" = "$o2" ] || { echo "FAIL: the rpl overlay's panel is not deterministic"; exit 1; }
r=$(dodag rpl "2:1 3:2 4:3 5:4" "$o1") || { echo "FAIL: rpl overlay on the Sky chain"; exit 1; }
echo "PLUGIN OK: rpl overlay, Sky chain from firmware memory: $r (deterministic)"
pk=$(sed 's/^plugins: \[rpl\]/plugins: [rpl-packets]/' "$CHAIN" > "$CHAIN.pk.yaml" && panels "$CHAIN.pk.yaml")
rm -f "$CHAIN.pk.yaml"
r=$(dodag rpl-packets "2:1 3:2 4:3 5:4" "$pk") || { echo "FAIL: rpl-packets overlay on the Sky chain"; exit 1; }
echo "PLUGIN OK: rpl-packets overlay, the same DODAG from DIOs and DAOs alone: $r"
r=$(dodag rpl "2:1 3:1" "$(panels configs/test-rpl-overlay-mixed.yaml)") ||
    { echo "FAIL: rpl overlay on Sky + CC2538 + nRF52840"; exit 1; }
echo "PLUGIN OK: rpl overlay, Sky + CC2538 + nRF52840 (two layouts, one DODAG): $r"
# Reads only: the overlay does not change the run.
tmp=$(mktemp -d "${TMPDIR:-/tmp}/csim-overlay-XXXXXX")
sed '/^plugins:/d' "$CHAIN" > "$tmp/none.yaml"
sed -i.bak "s|firmware: firmware/|firmware: $PWD/firmware/|" "$tmp/none.yaml"
"$RUNNER" test "$tmp/none.yaml" --pcap "$tmp/none.pcap" -q >/dev/null 2>&1
"$RUNNER" test "$CHAIN" --pcap "$tmp/rpl.pcap" -q >/dev/null 2>&1
if [ ! -s "$tmp/none.pcap" ] || ! cmp -s "$tmp/none.pcap" "$tmp/rpl.pcap"; then
    echo "FAIL: the rpl overlay changed the run (PCAPs differ)"; rm -rf "$tmp"; exit 1
fi
rm -rf "$tmp"
echo "PLUGIN OK: rpl overlay reads only (the same PCAP with and without it)"
exit 0
