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
# Then each scenario's "files" list must hold every file the page will read.
#
#   ./check-parity.sh                 # the default scenarios
#   ./check-parity.sh <runner args>   # one scenario of your own
set -u
cd "$(dirname "$0")/.."
NATIVE=build/test_runner
BUILDS="${PARITY_BUILDS:-wasm/dist/csim-cli.js wasm/dist/csim-ui-node.js}"
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
    # A scenario that fails natively proves nothing by failing the same way
    # in the browser build.
    if [ "$rn" != 0 ]; then
        echo "FAILED natively (exit $rn): $*"; tail -20 "$OUT/n.err"
        return 1
    fi
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
# Every scenario the site runs live, one per line: its args, then its "check"
# args.  Read before the loop: from a process substitution a bad
# scenarios.json would just mean no scenarios, and a pass.
list=$(python3 -c '
import json
for sc in json.load(open("wasm/web/scenarios.json")).values():
    print(" ".join(sc["args"] + sc.get("check", [])))') && [ -n "$list" ] ||
    { echo "error: could not read the scenarios from web/scenarios.json" >&2; exit 2; }
while IFS= read -r line; do
    read -r -a args <<< "$line"
    one "${args[@]}" || fail=1
done <<< "$list"

# The page reads only the files a scenario lists, but both Node builds read
# the repository, so parity alone passes with a firmware missing from the
# list.  Every file a scenario's args name, and every firmware its config
# names (read back through the config writer, so YAML and JSON alike), must
# be listed.
python3 - "$NATIVE" "$OUT/conv.yaml" <<'PY' || fail=1
import json, os, re, subprocess, sys
native, conv = sys.argv[1:]
bad = 0
for name, sc in json.load(open("wasm/web/scenarios.json")).items():
    need = [a for a in sc["args"][1:] if os.path.isfile(a)]
    named = []      # the paths the converted config names, as it writes them
    if sc["args"][0] == "test":
        if subprocess.run([native, "config-convert", sc["args"][1], conv],
                          stdout=subprocess.DEVNULL).returncode != 0:
            print("FILES %s: cannot read %s" % (name, sc["args"][1])); bad = 1; continue
        # Unanchored on purpose: it matches secure_firmware: too, which the
        # TrustZone scenario needs.  A quoted path fails the check loudly.
        text = open(conv).read()
        named += re.findall(r"firmware: ([^,} ]+)", text)
        # The overlay descriptors the config lists are files the page needs too.
        ov = re.search(r"^overlays:\n((?:  - .*\n)+)", text, re.M)
        if ov:
            named += [l[4:].strip() for l in ov.group(1).splitlines()]
    # The config writer keeps a path the config named relative to the cwd
    # as it was, and names one the config named relative to itself (an
    # example's ../firmware/...) relative to the file it writes, conv under
    # $TMPDIR, when their real paths share a leading directory (absolute
    # otherwise).  So a path that names a file from conv's real directory is
    # that file; any other is as written.  The files list is repo-relative:
    # compare real paths relative to the repo (the cwd; /tmp and /var are
    # links on macOS).
    here, at = os.path.realpath("."), os.path.dirname(os.path.realpath(conv))
    for f in named:
        g = os.path.join(at, f)
        need.append(os.path.relpath(os.path.realpath(g if os.path.exists(g) else f), here))
    for f in dict.fromkeys(need):
        if f not in sc["files"]:
            print("FILES %s: %s is not in its files list" % (name, f)); bad = 1
sys.exit(bad)
PY
exit $fail
