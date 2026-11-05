#!/bin/bash
#
# check-energest.sh — the energest plugin's checks (docs/energest.md
# "Validation"):
#
#   1. Time in state is exact and its files agree: a Sky pair's CSV, UI
#      frames and trace (tools/check-energest-output.py: every component's
#      states sum to its node's lifetime, the trace re-sums to the CSV, the
#      frames are well formed and end on the CSV's totals); the same again on
#      Sky + CC2538 + nRF52840 in one network.
#   2. The cross-checks the plugin prints hold: the CPU's low-power time is
#      the core's own fast-forward total (lpm_ns) to under a microsecond per
#      wake; every frame's air time is its bytes on air x the byte time; a
#      CC2420 TX lasts its frame plus a steady overhead of at least the
#      chip's 12-symbol (192 us) calibration.
#   3. Deterministic, and it only observes: two runs give the same files;
#      a run gives the same PCAP with the plugin and without it.
#   4. It agrees with the firmware's own Energest (simple-energest): CC2538
#      and nRF52840 rpl-udp, tx and listen within 2 rtimer ticks per 10 s;
#      the Sky's tx within 2, its listen within 12
#      (tools/check-energest-agreement.py).
#   5. A config it cannot honour is refused before the run (exit 2).
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
(cd "$tmp/a" && "$RUNNER" test "$tmp/sky.yaml" -q > out.txt 2>&1) || fail "the Sky pair did not run"
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
if len(a) != 2 or any(x != "equal to" for x in a):
    print("air time vs bytes:", a); bad = 1
for n, mn, mx in re.findall(r"(\d+) data frames: min ([0-9.-]+) max ([0-9.-]+)", out):
    mn, mx = float(mn), float(mx)
    if mn < 192.0 or mx - mn > 16.0:
        print(f"CC2420 tx dwell - air time: min {mn} max {mx} us"); bad = 1
if "CHECK FAILED" in out: bad = 1
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
(cd "$tmp/m" && "$RUNNER" test "$tmp/mixed.yaml" -q > out.txt 2>&1) || fail "the mixed network did not run"
"$PY" tools/check-energest-output.py "$tmp/m/energest.csv" "$tmp/m/energest.frames" \
    "$tmp/m/energest.trace" || fail "the mixed network's files"
grep -q "CHECK FAILED\|NOT equal" "$tmp/m/out.txt" && fail "a cross-check in the mixed network"
w=$(sed -n 's/.*cpu low-power .* (\([+-][0-9.]*\) us each)/\1/p' "$tmp/m/out.txt" | tr '\n' ' ')
for x in $w; do
    awk -v x="$x" 'BEGIN { exit !(x > -1 && x < 1) }' || fail "mixed: cpu low-power vs lpm_ns $x us per wake"
done
echo "energest OK: Sky + CC2538 + nRF52840, cpu low-power = lpm_ns to $w us per wake"

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
echo "energest OK: an unknown board, component, state or a bad number is refused (exit 2)"
echo "check-energest: OK"
