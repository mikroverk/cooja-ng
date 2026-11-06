#!/bin/sh
# check-lifecycle.sh — a reboot whose boot fails leaves the run sound.
#
# A node is rebooted from the shell after its firmware has gone or been
# truncated.  The reboot must fail with a message, the node must be left
# removed (not half-alive), the other nodes must run on, a later reboot from
# the restored file must bring the node back, and the run must end without
# a crash.  Under a sanitizer build (RUNNER=...) that also means no
# use-after-free or double free: the slot's platform was released by the
# failed boot and nothing may reach it again.
#
# Covered: MSP430 (Sky) and ARM (CC2538), a missing and a malformed image,
# and an RPL root lost under radio traffic.  Fixtures are temporary copies;
# the bundled firmware is never touched.
#
# Also: an ARM node's console lines written while it boots carry the time it
# boots at (0, then the reboot's time), so a node's lines never run
# backwards across its boot (Zephyr's banner on the CC1312R7 read 0.005,
# the run's first line after it 0.001).
#
# Usage: tools/check-lifecycle.sh   (RUNNER=... to override)
set -u
cd "$(dirname "$0")/.."
RUNNER=${RUNNER:-./build/test_runner}
W=$(mktemp -d "${TMPDIR:-/tmp}/check-lifecycle.XXXXXX")
trap 'rm -rf "$W"' EXIT
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}; export ASAN_OPTIONS
fails=0
ok()  { printf '  ok    %s\n' "$1"; }
bad() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }

# session NAME CFG -- feeds stdin from the function's own output; the
# commands wait for the nodes to boot before touching the files.
booted() { n=0; until grep -q "Initializing node 2" "$1" 2>/dev/null || [ $n -ge 200 ]; do sleep 0.1; n=$((n + 1)); done; }
check_run() {   # name: the run must not crash and must say the reboot failed
    out="$W/$1.out"
    if grep -qE "AddressSanitizer|runtime error:" "$out"; then
        bad "$1: $(grep -m1 -E 'ERROR: AddressSanitizer|runtime error:' "$out")"; return 1
    fi
    if [ "$2" -ge 128 ]; then bad "$1: killed by a signal (exit $2)"; return 1; fi
    if ! grep -q "node 1: reboot failed; the node is removed" "$out"; then
        bad "$1: no 'reboot failed' message"; return 1
    fi
    return 0
}

echo "== a Sky node rebooted from a firmware file that is gone"
cp firmware/sky/hello-world.sky "$W/a.sky"; cp firmware/sky/hello-world.sky "$W/b.sky"
printf 'nodes:\n  - { firmware: "%s/a.sky", id: 1 }\n  - { firmware: "%s/b.sky", id: 2 }\n' "$W" "$W" > "$W/sky.yaml"
{ booted "$W/sky-gone.out"; rm "$W/a.sky"; echo "reboot 1"; echo "nodes"; echo "run 11s"; echo "quit"; } \
    | "$RUNNER" test "$W/sky.yaml" --shell > "$W/sky-gone.out" 2>&1
rc=$?
if check_run sky-gone $rc; then
    if grep -E '^ +1 +NONE +removed' "$W/sky-gone.out" > /dev/null; then ok "node 1 is listed as removed"
    else bad "node 1 is not listed as removed"; fi
    if grep -E '^ +1[0-9]\.[0-9]{3} \[Node 2/MSP430\].*Hello, world' "$W/sky-gone.out" > /dev/null
    then ok "node 2 runs on (prints at 10 s)"; else bad "node 2 stopped"; fi
fi

echo "== a truncated image, then the restored file: the second reboot revives the node"
cp firmware/sky/hello-world.sky "$W/a.sky"
{ booted "$W/sky-bad.out"; head -c 300 firmware/sky/hello-world.sky > "$W/a.sky"; echo "reboot 1";
  echo "run 1s"; cp firmware/sky/hello-world.sky "$W/a.sky"; echo "reboot 1"; echo "run 1s"; echo "quit"; } \
    | "$RUNNER" test "$W/sky.yaml" --shell > "$W/sky-bad.out" 2>&1
rc=$?
if check_run sky-bad $rc; then
    if awk '/reboot failed/ { f = 1 } f && /\[Node 1\/MSP430\].*Hello, world/ { ok = 1 } END { exit !ok }' "$W/sky-bad.out"
    then ok "node 1 prints again after the reboot from the restored file"
    else bad "node 1 did not come back"; fi
fi

echo "== an ARM (CC2538) node rebooted from a firmware file that is gone"
cp firmware/cc2538dk/hello-world.cc2538dk "$W/a.cc2538dk"; cp firmware/cc2538dk/hello-world.cc2538dk "$W/b.cc2538dk"
printf 'nodes:\n  - { firmware: "%s/a.cc2538dk", id: 1 }\n  - { firmware: "%s/b.cc2538dk", id: 2 }\n' "$W" "$W" > "$W/arm.yaml"
{ booted "$W/arm-gone.out"; rm "$W/a.cc2538dk"; echo "reboot 1"; echo "run 2s"; echo "quit"; } \
    | "$RUNNER" test "$W/arm.yaml" --shell > "$W/arm-gone.out" 2>&1
rc=$?
check_run arm-gone $rc && ok "the run ends cleanly (exit $rc)"

echo "== the RPL root lost under radio traffic"
cp firmware/sky/udp-server.sky "$W/srv.sky"; cp firmware/sky/udp-client.sky "$W/cli.sky"
printf 'nodes:\n  - { firmware: "%s/srv.sky", id: 1, x: 0, y: 0 }\n  - { firmware: "%s/cli.sky", id: 2, x: 20, y: 0 }\n' "$W" "$W" > "$W/rpl.yaml"
{ booted "$W/rpl.out"; echo "run 30s"; rm "$W/srv.sky"; echo "reboot 1"; echo "run 30s"; echo "quit"; } \
    | "$RUNNER" test "$W/rpl.yaml" --shell > "$W/rpl.out" 2>&1
rc=$?
if check_run rpl $rc; then
    if grep -E '^ +[3-5][0-9]\.[0-9]{3} \[Node 2/MSP430\]' "$W/rpl.out" > /dev/null
    then ok "node 2 runs on without its root"; else bad "node 2 stopped"; fi
fi

echo "== boot lines carry the boot's time: Zephyr on the CC1312R7, booted and rebooted"
printf 'nodes:\n  - { firmware: "%s/firmware/cc1312r7-launchpad/zephyr-hello-world.cc1312r7-launchpad", id: 1 }\n' \
    "$PWD" > "$W/r7.yaml"
{ echo "run 1s"; echo "reboot 1"; echo "run 1s"; echo "quit"; } \
    | "$RUNNER" test "$W/r7.yaml" --shell > "$W/r7.out" 2>&1
grep -E '^ +[0-9]+\.[0-9]{3} \[Node 1/' "$W/r7.out" | awk '{ print $1 }' > "$W/r7.t"
if [ "$(tr '\n' ' ' < "$W/r7.t")" = "0.000 0.001 1.000 1.001 " ]; then
    ok "banner and hello at 0.000/0.001, again at 1.000/1.001 after the reboot"
else
    bad "boot-line stamps: $(tr '\n' ' ' < "$W/r7.t")(want 0.000 0.001 1.000 1.001)"
fi

echo
if [ "$fails" -eq 0 ]; then echo "check-lifecycle: OK"; else echo "check-lifecycle: $fails failed"; fi
[ "$fails" -eq 0 ]
