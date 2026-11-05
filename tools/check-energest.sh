#!/bin/bash
#
# check-energest.sh — the energest plugin's checks (docs/energest.md
# "Validation"):
#
#   1. Time in state is exact and its files agree: a Sky pair's CSV, UI
#      frames and trace (tools/check-energest-output.py: every component's
#      states sum to its node's lifetime, the trace re-sums to the CSV, the
#      frames are well formed and end on the CSV's totals); the same again on
#      Sky + CC2538 + nRF52840 in one network, and on both with nodes added,
#      removed and rebooted during the run (each life counted from its start,
#      with the lanes its new boot left on: tools/check-energest-summary.py).
#   2. The cross-checks the plugin prints hold: the CPU's low-power time is
#      the core's own fast-forward total (lpm_ns) to under a microsecond per
#      wake; every frame's air time is its bytes on air x the byte time; a
#      CC2420 TX lasts its frame plus a steady overhead of at least the
#      chip's 12-symbol (192 us) calibration.  A failed cross-check, a TX
#      shorter than its frame (but on the nRF54L15, whose radio lane is
#      flagged approximate) or an output file it cannot write fails the run
#      (exit 1; the engine unit suite: test_runner energest).
#   3. Deterministic, and it only observes: two runs give the same files;
#      a run gives the same PCAP with the plugin and without it.
#   4. It agrees with the firmware's own Energest (simple-energest): CC2538
#      and nRF52840 rpl-udp, tx and listen within 2 rtimer ticks per 10 s;
#      the Sky's tx within 2, its listen within 12
#      (tools/check-energest-agreement.py).
#   5. A config it cannot honour is refused before the run (exit 2), an
#      output path it cannot create too.
#
#   tools/check-energest.sh            # RUNNER=./build/test_runner by default
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
RUNNER=${RUNNER:-./build/test_runner}
RUNNER=$(cd "$(dirname "$RUNNER")" && pwd)/$(basename "$RUNNER")
PY=${PYTHON:-python3}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/csim-energest-XXXXXX")
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*"; exit 1; }
# run DIR CONFIG [ARGS...]: a run in DIR, its output in DIR/out.txt.  A
# failed cross-check or output file fails it (exit 1): the exit code is the
# verdict, and the summary lines say which.
run() {
    local d=$1 cfg=$2 rc; shift 2
    (cd "$d" && "$RUNNER" test "$cfg" -q "$@" > out.txt 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { grep -E "CHECK FAILED|NOT equal|energest: writing|energest: cannot" \
                             "$d/out.txt"; echo "exit $rc"; }
    return "$rc"
}

sky_cfg() {  # $1 = file, $2 = extra energest options, $3 = plugins line
    cat > "$1" <<EOF
version: 2
title: "energest check: a Sky pair"
timeout_ms: ${SKY_MS:-60000}
seed: 42
startup_delay_ms: 1000
$3
energest: { csv: energest.csv, frames: energest.frames, trace: energest.trace$2 }
mote_types:
  - { name: server, firmware: $ROOT/firmware/sky/udp-server.sky }
  - { name: client, firmware: $ROOT/firmware/sky/udp-client.sky }
nodes:
  - { type: server, id: 1, x: 0, y: 0 }
  - { type: client, id: 2, x: 30, y: 0 }
EOF
}

# --- 1 + 2: a Sky pair ---------------------------------------------------
mkdir -p "$tmp/a" "$tmp/b"
sky_cfg "$tmp/sky.yaml" ", battery_mah: 2600" "plugins: [energest]"
"$RUNNER" energest > "$tmp/unit.txt" 2>&1 || { cat "$tmp/unit.txt"; fail "the engine unit suite"; }
run "$tmp/a" "$tmp/sky.yaml" || fail "the Sky pair"
"$PY" tools/check-energest-output.py "$tmp/a/energest.csv" "$tmp/a/energest.frames" \
    "$tmp/a/energest.trace" || fail "the Sky pair's files"
"$PY" - "$tmp/a/out.txt" <<'EOF' || fail "the Sky pair's cross-checks"
import re, sys
out = open(sys.argv[1]).read()
bad = 0
w = [float(x) for x in re.findall(r"cpu low-power .* \(([+-][0-9.]+) us each\)", out)]
if len(w) != 2 or any(abs(x) >= 1.0 for x in w):
    print("cpu low-power vs lpm_ns per wake:", w); bad = 1
a = re.findall(r"bytes on air\): (.*) bytes x byte time", out)
if len(a) != 2:
    print("air time vs bytes:", a); bad = 1
for n, mn, mx in re.findall(r"(\d+) data frames: min ([0-9.-]+) max ([0-9.-]+)", out):
    mn, mx = float(mn), float(mx)
    if mn < 192.0 or mx - mn > 16.0:
        print(f"CC2420 tx dwell - air time: min {mn} max {mx} us"); bad = 1
print("cross-checks: cpu low-power = lpm_ns to %s us per wake; air time = bytes x 32 us; "
      "tx = frame + calibration" % "/".join("%+.3f" % x for x in w))
sys.exit(bad)
EOF

# --- 3: deterministic, and it only observes -----------------------------
(cd "$tmp/b" && "$RUNNER" test "$tmp/sky.yaml" -q > out.txt 2>&1)
for f in energest.csv energest.frames energest.trace; do
    cmp -s "$tmp/a/$f" "$tmp/b/$f" || fail "two runs wrote different $f"
done
echo "energest OK: deterministic (csv, frames and trace identical across runs)"
SKY_MS=20000 sky_cfg "$tmp/on.yaml" "" "plugins: [energest]"
SKY_MS=20000 sky_cfg "$tmp/off.yaml" "" ""
sed -i.bak '/^energest:/d' "$tmp/off.yaml"
(cd "$tmp/a" && "$RUNNER" test "$tmp/on.yaml" --pcap "$tmp/on.pcap" -q >/dev/null 2>&1)
(cd "$tmp/a" && "$RUNNER" test "$tmp/off.yaml" --pcap "$tmp/off.pcap" -q >/dev/null 2>&1)
[ -s "$tmp/off.pcap" ] && cmp -s "$tmp/on.pcap" "$tmp/off.pcap" ||
    fail "the energest plugin changed the run (PCAPs differ)"
echo "energest OK: reads only (the same PCAP with and without it)"

# --- 1 again: three platforms in one network -----------------------------
mkdir -p "$tmp/m"
cat > "$tmp/mixed.yaml" <<EOF
version: 2
title: "energest check: Sky + CC2538 + nRF52840"
timeout_ms: 40000
seed: 42
startup_delay_ms: 1000
plugins: [energest]
energest: { csv: energest.csv, frames: energest.frames, trace: energest.trace }
mote_types:
  - { name: sky, firmware: $ROOT/firmware/sky/udp-server.sky }
  - { name: cc, firmware: $ROOT/firmware/cc2538dk/udp-client.cc2538dk }
  - { name: nrf, firmware: $ROOT/firmware/nrf52840-dk/udp-client.nrf52840-dk }
nodes:
  - { type: sky, id: 1, x: 0, y: 0 }
  - { type: cc, id: 2, x: 20, y: 0 }
  - { type: nrf, id: 3, x: 40, y: 0 }
EOF
run "$tmp/m" "$tmp/mixed.yaml" || fail "the mixed network"
"$PY" tools/check-energest-output.py "$tmp/m/energest.csv" "$tmp/m/energest.frames" \
    "$tmp/m/energest.trace" || fail "the mixed network's files"
w=$(sed -n 's/.*cpu low-power .* (\([+-][0-9.]*\) us each)/\1/p' "$tmp/m/out.txt" | tr '\n' ' ')
for x in $w; do
    awk -v x="$x" 'BEGIN { exit !(x > -1 && x < 1) }' || fail "mixed: cpu low-power vs lpm_ns $x us per wake"
done
echo "energest OK: Sky + CC2538 + nRF52840, cpu low-power = lpm_ns to $w us per wake"

# --- 1 again: nodes added, removed and rebooted during the run -----------
# Every life counts from where it begins (an added node from its add, a
# rebooted one from its reboot: its reset states, then what its new boot
# reports) to where it ends, and nothing between two lives.
mkdir -p "$tmp/l" "$tmp/ml"
printf 'run 5s\nadd client 9 60 0\nrun 5s\nremove 2\nrun 1s\nreboot 2\nrun 3s\nexit\n' \
    > "$tmp/life.cnsh"
run "$tmp/l" "$tmp/sky.yaml" --script "$tmp/life.cnsh" ||
    fail "the Sky pair with nodes added, removed and rebooted"
"$PY" tools/check-energest-output.py "$tmp/l/energest.csv" "$tmp/l/energest.frames" \
    "$tmp/l/energest.trace" || fail "the Sky lifecycle run's files"
# node 9 lives 5..14 s of the script; node 2's second life (11..14 s) has
# its timer and its radio on, as node 1 has all along.
"$PY" tools/check-energest-summary.py "$tmp/l/out.txt" 9:lifetime=9.0 9:ma~1 2:ma~1 \
    1:Timer_A.on=100 2:Timer_A.on=100 9:Timer_A.on=100 '2:radio.listen>99.5' \
    '9:radio.listen>99.5' || fail "the Sky lifecycle run's summary"
printf 'run 5s\nadd nrf 9 30 10\nrun 5s\nremove 2\nremove 3\nrun 1s\nreboot 2\nreboot 3\nrun 3s\nexit\n' \
    > "$tmp/mlife.cnsh"
run "$tmp/ml" "$tmp/mixed.yaml" --script "$tmp/mlife.cnsh" ||
    fail "the mixed network with nodes added, removed and rebooted"
"$PY" tools/check-energest-output.py "$tmp/ml/energest.csv" "$tmp/ml/energest.frames" \
    "$tmp/ml/energest.trace" || fail "the mixed lifecycle run's files"
"$PY" tools/check-energest-summary.py "$tmp/ml/out.txt" 3:lifetime=13.0 9:lifetime=9.0 9:ma~3 \
    3:rtc0.on=100 3:timer0.on=100 9:rtc0.on=100 9:timer0.on=100 2:gpt0.on=100 \
    '3:radio.listen>99.5' '9:radio.listen>99.5' '2:radio.listen>90' ||
    fail "the mixed lifecycle run's summary"
sed '/^energest:/d; /^plugins:/d' "$tmp/mixed.yaml" > "$tmp/mixed-off.yaml"
(cd "$tmp/ml" && "$RUNNER" test "$tmp/mixed.yaml" --script "$tmp/mlife.cnsh" --pcap "$tmp/ml-on.pcap" \
     -q >/dev/null 2>&1)
(cd "$tmp/ml" && "$RUNNER" test "$tmp/mixed-off.yaml" --script "$tmp/mlife.cnsh" \
     --pcap "$tmp/ml-off.pcap" -q >/dev/null 2>&1)
[ -s "$tmp/ml-off.pcap" ] && cmp -s "$tmp/ml-on.pcap" "$tmp/ml-off.pcap" ||
    fail "the energest plugin changed the lifecycle run (PCAPs differ)"
echo "energest OK: nodes added, removed and rebooted (Sky; Sky + CC2538 + nRF52840): each life from its start, its lanes as its boot left them"

# --- 1 again: more nodes than the old 64-slot table ----------------------
# Every slot the runner has (SIM_EQ_MAX_NODES) is counted; none is dropped.
mkdir -p "$tmp/big"
{ printf 'version: 2\ntitle: "energest check: 70 Skys"\ntimeout_ms: 2000\nseed: 42\n'
  printf 'plugins: [energest]\nenergest: { csv: energest.csv, frames: energest.frames, trace: energest.trace }\n'
  printf 'mote_types:\n  - { name: hw, firmware: %s/firmware/sky/hello-world.sky }\nnodes:\n' "$ROOT"
  for i in $(seq 1 70); do printf '  - { type: hw, id: %d, x: %d, y: 0 }\n' "$i" $((i * 10)); done
} > "$tmp/big.yaml"
run "$tmp/big" "$tmp/big.yaml" || fail "70 Skys"
"$PY" tools/check-energest-output.py "$tmp/big/energest.csv" "$tmp/big/energest.frames" \
    "$tmp/big/energest.trace" > /dev/null || fail "70 Skys: the files"
n=$(grep -c "^energest: node .* lifetime" "$tmp/big/out.txt")
[ "$n" -eq 70 ] || fail "70 Skys: $n nodes in the summary"
echo "energest OK: 70 nodes, all 70 in the summary and the files"

# --- 1 again: a long startup delay ---------------------------------------
# Each node's lanes open at its start gate, seconds into the run here; the
# frames go out by the kernel's clock, so every frame lists the nodes alive
# in it (check-energest-output.py, by the trace's lives) and no stamp lands
# in a frame already sent.
mkdir -p "$tmp/dl"
{ printf 'version: 2\ntitle: "energest check: three Skys, a 5 s startup spread"\n'
  printf 'timeout_ms: 20000\nseed: 42\nstartup_delay_ms: 5000\nplugins: [energest]\n'
  printf 'energest: { csv: energest.csv, frames: energest.frames, trace: energest.trace }\n'
  printf 'mote_types:\n  - { name: c, firmware: %s/firmware/sky/udp-client.sky }\nnodes:\n' "$ROOT"
  for i in 1 2 3; do printf '  - { type: c, id: %d, x: %d, y: 0 }\n' "$i" $((i * 10)); done
} > "$tmp/delay.yaml"
run "$tmp/dl" "$tmp/delay.yaml" || fail "three Skys with a 5 s startup spread"
"$PY" tools/check-energest-output.py "$tmp/dl/energest.csv" "$tmp/dl/energest.frames" \
    "$tmp/dl/energest.trace" || fail "a 5 s startup spread: the files"
"$PY" tools/check-energest-summary.py "$tmp/dl/out.txt" > /dev/null ||
    fail "a 5 s startup spread: $(grep 'energest: note: ' "$tmp/dl/out.txt")"
echo "energest OK: a 5 s startup spread, every frame with the nodes alive in it"

# --- 3 again: a recording keeps every frame ------------------------------
# One frame per ms of simulated time, 100 per UI tick: a --ui-record holds
# every one, the same as the frames file.
SKY_MS=5000 sky_cfg "$tmp/fast.yaml" ", bin_ms: 1, frame_ms: 1" "plugins: [energest]"
mkdir -p "$tmp/f"
run "$tmp/f" "$tmp/fast.yaml" --ui-record "$tmp/f/rec.json" || fail "frame_ms 1 with --ui-record"
"$PY" - "$tmp/f/rec.json" "$tmp/f/energest.frames" <<'EOF' || fail "frame_ms 1: the recording's frames"
import json, sys
rec = json.load(open(sys.argv[1]))
got = [f["data"] for d in rec["deltas"] for f in d.get("f", []) if f["id"] == "energest"]
want = [json.loads(l) for l in open(sys.argv[2]) if l.strip()]
print(f"frame_ms 1: the recording has {len(got)} frames, the frames file {len(want)}")
sys.exit(0 if got == want and len(want) > 4000 else 1)
EOF
echo "energest OK: a --ui-record keeps every frame (frame_ms 1)"

# --- 2 again: the nRF54L15, whose radio model ends TX 100 us in ----------
# Its TX is shorter than the frames it sends: the run passes, its radio lane
# flagged approximate in the summary and the frames (on any other board that
# fails the run: the unit suite).
mkdir -p "$tmp/n54"
cat > "$tmp/n54.yaml" <<EOF
version: 2
title: "energest check: an nRF54L15 pair"
timeout_ms: 20000
seed: 42
plugins: [energest]
energest: { csv: energest.csv, frames: energest.frames, trace: energest.trace }
mote_types:
  - { name: s, firmware: $ROOT/firmware/nrf54l15-dk/udp-server.nrf54l15-dk }
  - { name: c, firmware: $ROOT/firmware/nrf54l15-dk/udp-client.nrf54l15-dk }
nodes:
  - { type: s, id: 1, x: 0, y: 0 }
  - { type: c, id: 2, x: 20, y: 0 }
EOF
run "$tmp/n54" "$tmp/n54.yaml" || fail "the nRF54L15 pair"
[ "$(grep -c "^energest: node .*, radio APPROXIMATE" "$tmp/n54/out.txt")" -eq 2 ] &&
    grep -q '"c":"radio","base":"[a-z]*","approx":true' "$tmp/n54/energest.frames" ||
    fail "the nRF54L15 radio lane is not flagged approximate"
echo "energest OK: the nRF54L15's radio lane is flagged approximate (tx is the model's 100 us per frame)"

# --- 4: the firmware's own Energest --------------------------------------
"$PY" tools/check-energest-agreement.py --runner "$RUNNER" --quiet \
    --run configs/energest-agreement-cc2538dk.yaml --cpu-hz 16000000 ||
    fail "CC2538 energest agreement"
"$PY" tools/check-energest-agreement.py --runner "$RUNNER" --quiet \
    --run configs/energest-agreement-nrf52840-dk.yaml --tick-hz 62500 --cpu-hz 64000000 ||
    fail "nRF52840 energest agreement"
# The Sky: Contiki's CC2420 driver counts TX from SFD on (--tx-from-sfd 5),
# and its listening is up to 10.2 ticks per 10 s longer than the chip's
# (docs/energest.md, an open item): a looser listen bound guards the rest.
"$PY" tools/check-energest-agreement.py --runner "$RUNNER" --quiet \
    --run configs/energest-agreement-sky.yaml --cpu-hz 3900000 --tx-from-sfd 5 \
    --listen-tolerance 12 || fail "Sky energest agreement"
echo "energest OK: agrees with the firmware's own Energest"

# --- 5: refusals ---------------------------------------------------------
refuse() {  # $1 = energest options, $2 = expected message
    SKY_MS=1000 sky_cfg "$tmp/bad.yaml" "$1" "plugins: [energest]"
    out=$(cd "$tmp/a" && "$RUNNER" test "$tmp/bad.yaml" -q 2>&1); rc=$?
    [ "$rc" -eq 2 ] && echo "$out" | grep -q "$2" ||
        fail "energest options '$1': exit $rc, want 2 and '$2'"
}
refuse ", currents: { sky: { cpu.lpm9: 1.0 } }" "'cpu' has no state 'lpm9'"
refuse ", currents: { sky: { flux.on: 1.0 } }" "unknown component 'flux'"
refuse ", currents: { skyy: { cpu.active: 1.0 } }" "unknown board 'skyy'"
refuse ", bin_ms: 3, frame_ms: 1000" "multiple of bin_ms"
refuse ", currents: { sky: { cpu.active: -1 } }" "must be a number >= 0"
SKY_MS=1000 sky_cfg "$tmp/bad.yaml" "" "plugins: [energest]"
sed -i.bak "s#csv: energest.csv#csv: $tmp/no-such-dir/e.csv#" "$tmp/bad.yaml"
out=$(cd "$tmp/a" && "$RUNNER" test "$tmp/bad.yaml" -q 2>&1); rc=$?
[ "$rc" -eq 2 ] && echo "$out" | grep -q "energest.csv: cannot create" ||
    fail "a csv path that cannot be created: exit $rc, want 2 and 'energest.csv: cannot create'"
echo "energest OK: an unknown board, component, state, a bad number or a csv path it cannot create is refused (exit 2)"

# --- 2 again: an output file it cannot write fails the run ---------------
# A file-size limit (SIGXFSZ ignored: the write fails with EFBIG) that the
# trace outgrows; stdout goes through a pipe, which the limit does not touch.
SKY_MS=5000 sky_cfg "$tmp/small.yaml" "" "plugins: [energest]"
( trap '' XFSZ; ulimit -f 4; cd "$tmp/a" && exec "$RUNNER" test "$tmp/small.yaml" -q 2>&1 ) |
    cat > "$tmp/xfsz.txt"
rc=${PIPESTATUS[0]}
[ "$rc" -eq 1 ] && grep -q "energest: writing energest.trace failed" "$tmp/xfsz.txt" ||
    fail "a trace that could not be written: exit $rc, want 1 and 'writing energest.trace failed'"
echo "energest OK: an output file it cannot write fails the run (exit 1)"
echo "check-energest: OK"
