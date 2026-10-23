#!/usr/bin/env bash
# check-parity.sh — the WASM builds must simulate exactly what the native one
# does.  Runs each scenario with build/test_runner and with each WASM build
# under Node, and diffs stdout and stderr separately (their interleaving
# depends on host buffering, not on the simulation) after dropping the
# wall-clock lines tools/check-determinism.sh drops.  Exit codes must match
# too.
#
# The WASM builds compared:
#   dist/csim-cli.js      the plain build (a drop-in for build/test_runner)
#   dist/csim-ui-node.js  what the page runs: the web objects (ws_server_worker,
#                         web_main) and ASYNCIFY, under Node with the repo's files
#
# The scenarios: every live one in web/scenarios.json, with its "check" args
# (a bound on the run where the live page runs unbounded), plus timertest.
#
#   ./check-parity.sh                 # the default scenarios
#   ./check-parity.sh <runner args>   # one scenario of your own
set -u
cd "$(dirname "$0")/../.."
NATIVE=build/test_runner
BUILDS="${PARITY_BUILDS:-experiments/browser-wasm/dist/csim-cli.js experiments/browser-wasm/dist/csim-ui-node.js}"
[ -x "$NATIVE" ] || { echo "error: $NATIVE not built (run make)" >&2; exit 2; }
for b in $BUILDS; do
    [ -f "$b" ] || { echo "error: $b not built" >&2; exit 2; }
done

# One filter for wall-clock lines, kept in check-determinism.sh.
NOISE=$(sed -n "s/^NOISE='\(.*\)'$/\1/p" tools/check-determinism.sh)
[ -n "$NOISE" ] || { echo "error: no NOISE= line in tools/check-determinism.sh" >&2; exit 2; }
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT

one() {
    $NATIVE "$@" > "$OUT/n.out" 2> "$OUT/n.err"; local rn=$?
    local s
    for s in out err; do grep -avE "$NOISE" "$OUT/n.$s" > "$OUT/n.$s.f"; done
    local bad=0 b
    for b in $BUILDS; do
        node "$b" "$@" > "$OUT/w.out" 2> "$OUT/w.err"; local rw=$?
        for s in out err; do
            grep -avE "$NOISE" "$OUT/w.$s" > "$OUT/w.$s.f"
            if ! diff -u "$OUT/n.$s.f" "$OUT/w.$s.f" > "$OUT/d.$s"; then
                echo "DIVERGENCE ($(basename "$b"), $s): $*"; head -40 "$OUT/d.$s"; bad=1
            fi
        done
        [ "$rn" = "$rw" ] || { echo "EXIT CODE native=$rn $(basename "$b")=$rw: $*"; bad=1; }
    done
    [ $bad = 0 ] && echo "IDENTICAL ($(wc -l < "$OUT/n.out.f") + $(wc -l < "$OUT/n.err.f") lines, exit $rn, $(echo $BUILDS | wc -w | tr -d ' ') builds): $*"
    return $bad
}

if [ $# -gt 0 ]; then one "$@"; exit $?; fi
fail=0
one multinode firmware/sky/timertest.sky -t 10000 || fail=1
# Every scenario the site runs live, one per line: its args, then its "check" args.
while IFS= read -r line; do
    read -r -a args <<< "$line"
    one "${args[@]}" || fail=1
done < <(python3 -c '
import json
for sc in json.load(open("experiments/browser-wasm/web/scenarios.json")).values():
    print(" ".join(sc["args"] + sc.get("check", [])))')
exit $fail
