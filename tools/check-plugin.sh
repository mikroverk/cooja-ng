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
#      nRF52840 from two struct layouts, in a three-hop line across them
#      (examples/overlay-mixed-line.yaml) — deterministically, and without
#      changing the run (the same PCAP with and without it);
#   5. a restart keeps every service attached once: 18 restarts in a shell
#      session do not end the run (they filled the 16-slot service table and
#      exited 2 at the third), energest counts the restarted run alone, the
#      rpl overlay's layer is emptied at the restart and a restarted run's
#      panel is a fresh run's, and rpl-packets forgets a node rebooted on its own;
#   6. a plugin that does not ask for frames gets none headless (packet_sink:
#      0 frames, as before plugin ABI v4), and a .so gets on_restart and
#      wants_frames through register_service_v4 only (test/plugins/abi_probe.c).
#   7. the thread overlay draws OpenThread's leader and children, their
#      parent links and one partition, on CC2538s and on nRF52840s, and
#      survives 18 restarts with its layer emptied at each;
#   8. the overlay descriptor examples/overlays/thread.yaml draws the same
#      nodes and links as the thread overlay, and a broken descriptor stops
#      the run with exit 2.
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
# A v1 plugin does not ask for frames, so a headless run captures none for it.
echo "$t1" | grep -q '^packet-sink: 0 frames ' || { echo "FAIL: packet_sink got frames headless: $t1"; exit 1; }

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

# The nRF52840's RADIO reports its state too (it used to report none, so the
# plugin listed no nRF52840 mote and charged 0 mJ): both motes listed, the
# radio on and listening, some TX and RX, deterministic.
EN_NRF=configs/plugin-energest-nrf52840-dk.yaml
n1=$("$RUNNER" test "$EN_NRF" -t "$T" 2>&1 | grep -E '^energest:')
n2=$("$RUNNER" test "$EN_NRF" -t "$T" 2>&1 | grep -E '^energest:')
if [ "$n1" != "$n2" ]; then echo "FAIL: nRF52840 energest not deterministic"; exit 1; fi
nm=$(echo "$n1" | grep -cE '^energest:  mote ')
if [ "$nm" -ne 2 ]; then echo "FAIL: nRF52840 energest lists $nm motes, want 2"; echo "$n1"; exit 1; fi
if echo "$n1" | grep -E '^energest:  mote ' | grep -qE 'radio-on 0\.000s|tx 0\.000 |rx 0\.000 '; then
    echo "FAIL: nRF52840 energest: a mote with no radio-on, TX or RX time"; echo "$n1"; exit 1; fi
echo "PLUGIN OK: energest on the nRF52840 ($(echo "$n1" | grep -E '^energest:  mote 0' | sed 's/^energest: *//; s/  cpu.*//'))"

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
tmp=$(mktemp -d "${TMPDIR:-/tmp}/csim-overlay-XXXXXX")
sed -e 's/^plugins: \[rpl\]/plugins: [rpl-packets]/' -e "s|firmware: firmware/|firmware: $PWD/firmware/|" \
    "$CHAIN" > "$tmp/packets.yaml"
pk=$(panels "$tmp/packets.yaml")
r=$(dodag rpl-packets "2:1 3:2 4:3 5:4" "$pk") || { echo "FAIL: rpl-packets overlay on the Sky chain"; exit 1; }
echo "PLUGIN OK: rpl-packets overlay, the same DODAG from DIOs and DAOs alone: $r"
r=$(dodag rpl "2:1 3:1" "$(panels configs/test-rpl-overlay-mixed.yaml)") ||
    { echo "FAIL: rpl overlay on Sky + CC2538 + nRF52840"; exit 1; }
echo "PLUGIN OK: rpl overlay, Sky + CC2538 + nRF52840 (two layouts, one DODAG): $r"
r=$(dodag rpl "2:1 3:2" "$(panels examples/overlay-mixed-line.yaml)") ||
    { echo "FAIL: rpl overlay on examples/overlay-mixed-line.yaml"; exit 1; }
echo "PLUGIN OK: rpl overlay, examples/overlay-mixed-line.yaml (CC2538 -> nRF52840 -> Sky): $r"
# Reads only: the overlay does not change the run.
sed '/^plugins:/d' "$CHAIN" > "$tmp/none.yaml"
sed -i.bak "s|firmware: firmware/|firmware: $PWD/firmware/|" "$tmp/none.yaml"
"$RUNNER" test "$tmp/none.yaml" --pcap "$tmp/none.pcap" -q >/dev/null 2>&1
"$RUNNER" test "$CHAIN" --pcap "$tmp/rpl.pcap" -q >/dev/null 2>&1
if [ ! -s "$tmp/none.pcap" ] || ! cmp -s "$tmp/none.pcap" "$tmp/rpl.pcap"; then
    echo "FAIL: the rpl overlay changed the run (PCAPs differ)"; rm -rf "$tmp"; exit 1
fi
echo "PLUGIN OK: rpl overlay reads only (the same PCAP with and without it)"

# --- Restarts: services stay attached, and forget the old run ---
for i in $(seq 1 18); do printf 'run 300ms\nrestart\n'; done > "$tmp/restarts.cnsh"
printf 'run 300ms\nexit\n' >> "$tmp/restarts.cnsh"
for cfg in "$CHAIN" "$EN_CFG"; do
    "$RUNNER" test "$cfg" --shell -q < "$tmp/restarts.cnsh" > "$tmp/restarts.log" 2>&1
    rc=$?
    n=$(grep -c '^--- Restarting simulation' "$tmp/restarts.log")
    if [ "$rc" -ne 0 ] || [ "$n" -ne 18 ]; then
        echo "FAIL: 18 restarts of $cfg: exit $rc after $n restarts"; grep -m3 'plugin' "$tmp/restarts.log"
        rm -rf "$tmp"; exit 1
    fi
done
"$RUNNER" test "$CFG" --plugin "$SO" --shell -q < "$tmp/restarts.cnsh" > "$tmp/restarts.log" 2>&1
rc=$?   # 1: the config's test is not done in 300 ms; 2 would be a refused plugin
n=$(grep -c '^--- Restarting simulation' "$tmp/restarts.log")
if [ "$rc" -eq 2 ] || [ "$n" -ne 18 ]; then
    echo "FAIL: 18 restarts with --plugin $SO: exit $rc after $n restarts"; rm -rf "$tmp"; exit 1
fi
er=$("$RUNNER" test "$EN_CFG" --shell -q < "$tmp/restarts.cnsh" 2>&1 | grep '^energest:')
ef=$(printf 'run 300ms\nexit\n' | "$RUNNER" test "$EN_CFG" --shell -q 2>&1 | grep '^energest:')
if [ -z "$ef" ] || [ "$er" != "$ef" ]; then
    echo "FAIL: energest after 18 restarts is not a fresh 300 ms run's"; rm -rf "$tmp"; exit 1
fi
echo "PLUGIN OK: 18 restarts (rpl overlay, energest, a .so): no service attached twice; energest counts the last run alone"
# The v4 service members reach a .so only through register_service_v4
# (test/plugins/abi_probe.c): its probe-v1 sets them but registers through
# register_service, so it gets no restart and no frames.
PROBE=build/plugins/abi_probe.so
[ -f "$PROBE" ] || { echo "FAIL: $PROBE not built"; rm -rf "$tmp"; exit 1; }
pr=$("$RUNNER" test "$CFG" --plugin "$PROBE" --shell -q < "$tmp/restarts.cnsh" 2>&1 | grep '^abi-probe:' | sort)
want=$(printf 'abi-probe: probe-v1 0 frames, 0 restarts\nabi-probe: probe-v4 0 frames, 18 restarts')
[ "$pr" = "$want" ] || { echo "FAIL: on_restart through the v4 registration:"; echo "$pr"; rm -rf "$tmp"; exit 1; }
pf=$(ABI_PROBE_FRAMES=1 "$RUNNER" test "$CFG" -t 30000 --plugin "$PROBE" -q 2>&1 | grep '^abi-probe: probe-v4')
echo "$pf" | grep -qE 'probe-v4 [1-9][0-9]* frames' || { echo "FAIL: wants_frames gave no frames: $pf"; rm -rf "$tmp"; exit 1; }
echo "PLUGIN OK: v4 service members: on_restart and wants_frames through register_service_v4 only"
shellpanels() { CSIM_DUMP_PANELS=1 "$RUNNER" test "$CHAIN" --shell -q 2>&1 | sed -n 's/^UI panels: //p'; }
p=$(printf 'run 30s\nrestart\nexit\n' | shellpanels)
python3 -c '
import json, sys
L = json.loads(sys.argv[1])["rpl"]
assert L["layer"]["nodes"] == {} and L["layer"]["links"] == [], "the old DODAG is still drawn: %r" % L
' "$p" || { echo "FAIL: the rpl layer is not emptied at a restart"; rm -rf "$tmp"; exit 1; }
pf=$(printf 'run 30s\nexit\n' | shellpanels)
pr=$(printf 'run 30s\nrestart\nrun 30s\nexit\n' | shellpanels)
if [ -z "$pf" ] || [ "$pf" != "$pr" ]; then
    echo "FAIL: the rpl overlay after a restart is not a fresh run's"; rm -rf "$tmp"; exit 1
fi
echo "PLUGIN OK: rpl overlay emptied at a restart, and a restarted run's panel is a fresh run's"
# A node rebooted on its own: the packet view forgets what its packets said.
pk=$(printf 'run 90s\nreboot 5\nrun 600ms\nexit\n' | CSIM_DUMP_PANELS=1 "$RUNNER" test "$tmp/packets.yaml" --shell -q 2>&1 |
     sed -n 's/^UI panels: //p')
python3 -c '
import json, sys
L = json.loads(sys.argv[1])["rpl-packets"]
assert "5" not in L["layer"]["nodes"] and "no RPL seen yet: node 5" in L["text"], "node 5 still drawn: %r" % L
' "$pk" || { echo "FAIL: rpl-packets kept a rebooted node's packets"; rm -rf "$tmp"; exit 1; }
echo "PLUGIN OK: rpl-packets forgets a node rebooted on its own"
rm -rf "$tmp"

# --- The thread overlay (OpenThread roles from ot::gInstanceRaw) ---
thread_check() {  # thread_check CONFIG "CHILD..." -> what it found
    local cfg=$1 kids=$2 t
    t=$(mktemp "${TMPDIR:-/tmp}/csim-thread-XXXXXX") && mv "$t" "$t.yaml" && t="$t.yaml"
    sed 's/^timeout_ms: \(.*\)/timeout_ms: \1\nplugins: [thread]/' "$cfg" |
        sed "s|firmware: firmware/|firmware: $PWD/firmware/|" > "$t"
    panels "$t" | python3 -c '
import json, sys
L = json.loads(sys.stdin.read())["thread"]["layer"]
n, links = L["nodes"], {(l["from"], l["to"]) for l in L["links"] if l["style"] == "parent"}
assert n["1"]["glyph"] == "hex" and n["1"]["badge"] == "L", "node 1 is not the leader: %r" % n.get("1")
kids = sys.argv[1].split()
for c in kids:
    assert n[c]["glyph"] == "circle", "node %s is not a child: %r" % (c, n[c])
assert links == {(int(c), 1) for c in kids}, "parent links %r" % links
assert len({x["fill"] for x in n.values()}) == 1, "not one partition"
assert any("state" in a[1] for a in n["2"].get("actions", [])), "no role commands"
print("leader 1, child%s %s, one partition" % ("ren" if len(kids) > 1 else "", " and ".join(kids)))
' "$kids"
    local rc=$?
    rm -f "$t"
    return $rc
}
for spec in "configs/test-openthread-cc2538dk.yaml:2 3" "configs/test-openthread-nrf52840-dk.yaml:2"; do
    cfg=${spec%%:*}
    r=$(thread_check "$cfg" "${spec#*:}") || { echo "FAIL: thread overlay on $cfg"; exit 1; }
    echo "PLUGIN OK: thread overlay, $(basename "$cfg" .yaml): $r"
done
# Restarts with the thread overlay (examples/thread-overlay.yaml, six
# CC2538s, drawn by the descriptor examples/overlays/thread.yaml): 18 in a
# shell session end with exit 0, and the layer is emptied at a restart.
th=$(mktemp -d "${TMPDIR:-/tmp}/csim-thread-XXXXXX")
for i in $(seq 1 18); do printf 'run 300ms\nrestart\n'; done > "$th/restarts.cnsh"
printf 'run 300ms\nexit\n' >> "$th/restarts.cnsh"
"$RUNNER" test examples/thread-overlay.yaml --shell -q < "$th/restarts.cnsh" > "$th/restarts.log" 2>&1
rc=$?
n=$(grep -c '^--- Restarting simulation' "$th/restarts.log")
if [ "$rc" -ne 0 ] || [ "$n" -ne 18 ]; then
    echo "FAIL: 18 restarts of examples/thread-overlay.yaml: exit $rc after $n restarts"; rm -rf "$th"; exit 1
fi
p=$(printf 'run 20s\nrestart\nexit\n' | CSIM_DUMP_PANELS=1 "$RUNNER" test examples/thread-overlay.yaml --shell -q 2>&1 |
    sed -n 's/^UI panels: //p')
python3 -c '
import json, sys
L = json.loads(sys.argv[1])["thread-descriptor"]   # the example draws the descriptor
assert L["layer"]["nodes"] == {} and "restarted" in L["text"], "the old network is still drawn: %r" % L
' "$p" || { echo "FAIL: the thread layer is not emptied at a restart"; rm -rf "$th"; exit 1; }
rm -rf "$th"
echo "PLUGIN OK: thread overlay descriptor: 18 restarts (exit 0), the layer emptied at a restart"

# --- Overlay descriptors: the thread overlay as a file, the same picture ---
# Both overlays on one run, compared panel for panel (nodes with every
# attribute and action, links with style and colour).  Twice: the leader
# and two children of test-openthread-cc2538dk, and the same three with
# nodes 2 and 3 made routers (router links from the router-table list,
# drawn once per pair), run past the test's own end.
t=$(mktemp "${TMPDIR:-/tmp}/csim-desc-XXXXXX") && mv "$t" "$t.yaml" && t="$t.yaml"
sed 's/^timeout_ms: \(.*\)/timeout_ms: \1\nplugins: [thread]\noverlays: ['"$(printf '%s' "$PWD/examples/overlays/thread.yaml" | sed 's|/|\\/|g')"']/' \
    configs/test-openthread-cc2538dk.yaml | sed "s|firmware: firmware/|firmware: $PWD/firmware/|" > "$t"
python3 - "$t" "$t.r.yaml" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
s = re.sub(r"(?ms)^  steps:.*?^  actions:\n", "  actions:\n", s)          # no end on the ping
s = s.replace("timeout_ms: 25000", "timeout_ms: 34000")
s += ('    - { at_ms: 22000, type: send, node: 2, data: "state router\\n" }\n'
      '    - { at_ms: 22000, type: send, node: 3, data: "state router\\n" }\n')
open(sys.argv[2], "w").write(s)
PY
desc_equal() {  # desc_equal CONFIG MIN_ROUTER_LINKS -> "N nodes, M links"
    panels "$1" | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
def norm(L):
    return ({k: {x: v[x] for x in ("glyph", "fill", "ring", "ring_dash", "badge", "label", "actions") if x in v}
             for k, v in L["nodes"].items()},
            sorted((l["from"], l["to"], l["style"], l.get("color")) for l in L["links"]))
a, b = norm(d["thread"]["layer"]), norm(d["thread-descriptor"]["layer"])
assert a == b, "descriptor %r != thread overlay %r" % (b, a)
routers = [l for l in a[1] if l[2] == "router"]
assert len(a[0]) == 3 and a[1], "too little drawn to compare: %r" % (a,)
assert len(routers) >= int(sys.argv[1]), "router links %r, want at least %s" % (routers, sys.argv[1])
assert all(any("leaderweight" in c[1] for c in n.get("actions", [])) for n in a[0].values() if n.get("badge") == "R"), \
    "a router without become leader: %r" % (a[0],)
print("%d nodes, %d links (%d router)" % (len(a[0]), len(a[1]), len(routers)))
' "$2"
}
r=$(desc_equal "$t" 0) || { echo "FAIL: the descriptor does not draw what the thread overlay does"; rm -f "$t" "$t.r.yaml"; exit 1; }
echo "PLUGIN OK: overlay descriptor examples/overlays/thread.yaml = the thread overlay, a leader and children ($r)"
r=$(desc_equal "$t.r.yaml" 2) || { echo "FAIL: the descriptor does not draw what the thread overlay does (routers)"; rm -f "$t" "$t.r.yaml"; exit 1; }
rm -f "$t.r.yaml"
echo "PLUGIN OK: overlay descriptor examples/overlays/thread.yaml = the thread overlay, routers ($r)"
printf 'overlay: x\nsymbol: s\nstruct: S\nfields: { a: b }\nbogus: 1\n' > "$t.bad.yaml"
sed "s|^overlays: .*|overlays: [$t.bad.yaml]|" "$t" > "$t.2.yaml"
miss=$("$RUNNER" test "$t.2.yaml" -t 1000 -q 2>&1); rc=$?
rm -f "$t" "$t.bad.yaml" "$t.2.yaml"
[ "$rc" -eq 2 ] && echo "$miss" | grep -q "unknown key 'bogus'" ||
    { echo "FAIL: a broken descriptor gave rc=$rc: $miss"; exit 1; }
echo "PLUGIN OK: a broken descriptor stops the run (exit 2, with the reason)"
exit 0
