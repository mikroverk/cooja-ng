#!/usr/bin/env bash
# check-idle-skip.sh — idle-loop skipping must not change a run.
#
# Runs each setup twice, with ARM idle-loop skipping off (--no-idle-skip)
# and on (--idle-skip), and requires stdout, stderr, the exit code and the
# PCAP capture to be byte-identical; only the host's wall-clock lines
# (Wall-clock time / Speed ratio / Throughput) may differ.  Every console
# line carries a simulated timestamp and the end-of-run report gives each
# node's cycle and instruction counts and PC, so identical output means every
# node ran the same instructions at the same simulated nanoseconds.  See
# docs/idle-skip.md.
#
# Usage:
#   tools/check-idle-skip.sh                 # the default setups (CI)
#   tools/check-idle-skip.sh SETUP.yaml ...  # these setups
#   JIT=0 tools/check-idle-skip.sh           # with the ARM JIT off too
#   CHECK=1 tools/check-idle-skip.sh         # also run the landing model in
#                                            # check mode (CSIM_IDLE_SKIP_CHECK)
#   RUNNER=build-asan/test_runner tools/check-idle-skip.sh
#   TIMING=1 tools/check-idle-skip.sh        # off and on one after the other,
#                                            # for wall times worth quoting
#   T=6000 tools/check-idle-skip.sh          # each run's first 6 s only (-t)
#
# Exit 0 = identical everywhere; 1 = a difference (shown); 2 = usage/build.

set -u
cd "$(dirname "$0")/.." || exit 2

RUNNER=${RUNNER:-build/test_runner}
if [ ! -x "$RUNNER" ]; then
    echo "error: $RUNNER not built. Run 'make' first." >&2
    exit 2
fi

if [ "$#" -gt 0 ]; then
    SETUPS=("$@")
else
    # The OpenThread setups (the polling firmware the option is for), the
    # examples that enable it, and Contiki-NG and Zephyr ARM runs, which
    # sleep in WFI and must come out the same (and no slower).
    SETUPS=(
        configs/test-openthread-cc2538dk.yaml
        configs/test-openthread-nrf52840-dk.yaml
        examples/thread-cc2538.yaml
        examples/thread-nrf52840.yaml
        configs/chain-4node-cc2538dk.json
        configs/chain-3node-nrf52840-dk.json
        configs/interop-contiki-zephyr.json
        configs/test-idle-rtc-ovrflw-nrf52840.yaml
        configs/test-idle-blx-wild-nrf52840.yaml
    )
fi

FILTER='^ *(Wall-clock time|Speed ratio|Throughput):'
WORK=$(mktemp -d "${TMPDIR:-/tmp}/csim-idle-skip.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

export CSIM_ARM_JIT=${JIT:-${CSIM_ARM_JIT:-1}}

wall() { sed -n 's/^ *Wall-clock time: *\([0-9.]*\) ms.*/\1/p' "$1" | head -1; }

run_one() {   # run_one DIR SETUP FLAG [env...]
    local dir=$1 setup=$2 flag=$3; shift 3
    mkdir -p "$dir"
    env "$@" "$RUNNER" test "$setup" -q "$flag" ${T:+-t "$T"} --pcap "$dir/capture.pcap" \
        >"$dir/stdout" 2>"$dir/stderr"
    echo $? >"$dir/rc"
    # The capture path is the one thing the two runs are told differently.
    sed -i.bak "s#$dir/#CAPTURE/#g" "$dir/stdout" && rm -f "$dir/stdout.bak"
}

fail=0
for setup in "${SETUPS[@]}"; do
    if [ ! -f "$setup" ]; then
        echo "error: $setup: no such setup" >&2
        exit 2
    fi
    name=$(basename "$setup" | sed 's/\.[a-z]*$//')
    off="$WORK/$name/off" on="$WORK/$name/on"
    if [ "${TIMING:-0}" = 1 ]; then
        run_one "$off" "$setup" --no-idle-skip
        run_one "$on" "$setup" --idle-skip
    else
        run_one "$off" "$setup" --no-idle-skip &
        run_one "$on" "$setup" --idle-skip &
        wait
    fi
    bad=""
    [ "$(cat "$off/rc")" = "$(cat "$on/rc")" ] || bad="$bad exit-code($(cat "$off/rc")/$(cat "$on/rc"))"
    diff <(grep -vE "$FILTER" "$off/stdout") <(grep -vE "$FILTER" "$on/stdout") \
        >"$WORK/$name.stdout.diff" || bad="$bad stdout"
    diff "$off/stderr" "$on/stderr" >"$WORK/$name.stderr.diff" || bad="$bad stderr"
    if [ -f "$off/capture.pcap" ] || [ -f "$on/capture.pcap" ]; then
        cmp -s "$off/capture.pcap" "$on/capture.pcap" || bad="$bad pcap"
    fi
    w_off=$(wall "$off/stdout") w_on=$(wall "$on/stdout")
    ratio=$(awk -v a="${w_off:-0}" -v b="${w_on:-0}" 'BEGIN { if (b > 0) printf "%.2fx", a / b; else print "-" }')
    if [ -n "$bad" ]; then
        printf "  DIFF  %-34s off %9s ms  on %9s ms  (%s)\n" "$name" "$w_off" "$w_on" "${bad# }"
        head -20 "$WORK/$name.stdout.diff" "$WORK/$name.stderr.diff" | sed 's/^/        /'
        fail=1
    else
        printf "  ok    %-34s off %9s ms  on %9s ms  %s\n" "$name" "$w_off" "$w_on" "$ratio"
    fi
    if [ "${CHECK:-0}" = 1 ]; then
        chk="$WORK/$name/check"
        run_one "$chk" "$setup" --idle-skip CSIM_IDLE_SKIP_CHECK=1 CSIM_IDLE_SKIP_STATS=1
        mism=$(sed -n 's/.*mismatches=\([0-9]*\).*/\1/p' "$chk/stderr" | head -1)
        checked=$(sed -n 's/.*slices-checked=\([0-9]*\).*/\1/p' "$chk/stderr" | head -1)
        if [ "${mism:-0}" != 0 ]; then
            printf "  MODEL %-34s %s of %s checked slices mispredicted\n" "$name" "$mism" "$checked"
            fail=1
        else
            printf "  model %-34s %s slices checked, none mispredicted\n" "$name" "${checked:-0}"
        fi
    fi
done
# A saved setup is the setup as it ran: --no-idle-skip on a setup that turns
# it on saves no idle_skip, and --idle-skip on one without it saves it on.
"$RUNNER" test configs/test-idle-rtc-ovrflw-nrf52840.yaml -q --no-idle-skip \
    --save-config "$WORK/saved-off.yaml" >/dev/null 2>&1
"$RUNNER" test configs/chain-3node-nrf52840-dk.json -q -t 200 --idle-skip \
    --save-config "$WORK/saved-on.yaml" >/dev/null 2>&1      # a setup without the key
if [ ! -s "$WORK/saved-off.yaml" ] || [ ! -s "$WORK/saved-on.yaml" ] ||
   grep -q '^idle_skip:' "$WORK/saved-off.yaml" || ! grep -q '^idle_skip: true' "$WORK/saved-on.yaml"; then
    echo "  SAVE  --save-config does not keep the idle_skip the run used"
    fail=1
else
    echo "  ok    --save-config keeps the idle_skip the run used"
fi
(( fail )) && echo "check-idle-skip: FAILED" && exit 1
if [ "${TIMING:-0}" = 1 ]; then
    echo "check-idle-skip: OK"
else
    echo "check-idle-skip: OK (wall times from concurrent on/off pairs: indicative only; TIMING=1 serialises them)"
fi
exit 0
