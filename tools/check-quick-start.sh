#!/bin/sh
# check-quick-start.sh — gate G3: docs/quick-start.md, run as written.
#
# Against a package root (the release tarball's layout: bin/cngsim beside
# examples/, firmware/ and docs/), from an empty directory outside any
# checkout, with only the package's bin/ added to PATH:
#
#   1. Every line of the guide's ```sh blocks runs and exits 0, except what
#      needs the network or a Contiki-NG toolchain (the download, the clone,
#      `make`, `cd contiki-ng/...`), which is listed as skipped.  A `--ui`
#      line runs with --no-open --start --speed max, and the page it names
#      (http://127.0.0.1:8080/) must be the web UI.
#   2. The guide's transcripts are what the package prints: every line of the
#      `cngsim run rpl-udp-sky` output shown appears in the real output (the
#      verdict up to its wall-clock part), and every board row shown appears
#      in `cngsim boards`.
#   3. The guide's `cngsim check ...` and `cngsim help ...` mentions run.
#
# Usage: tools/check-quick-start.sh PKG [GUIDE]   (GUIDE defaults to PKG's
#        docs/quick-start.md, the copy the package ships)
set -u
[ $# -ge 1 ] || { echo "usage: $0 PKG [GUIDE]" >&2; exit 2; }
PKG=$(cd "$1" && pwd) || exit 2
GUIDE=${2:-$PKG/docs/quick-start.md}
[ -x "$PKG/bin/cngsim" ] || { echo "error: no $PKG/bin/cngsim" >&2; exit 2; }
[ -f "$GUIDE" ] || { echo "error: no $GUIDE" >&2; exit 2; }
GUIDE=$(cd "$(dirname "$GUIDE")" && pwd)/$(basename "$GUIDE")
PATH="$PKG/bin:$PATH"; export PATH
WORK=$(mktemp -d "${TMPDIR:-/tmp}/check-quick-start.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 2
fails=0 n=0

ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }
skip() { printf '  skip  %s\n' "$1"; }

# The guide's blocks of one kind, one line each, blocks separated by a
# line holding only "@@".
blocks() {
    awk -v want="$1" '
        /^```/ { if (open) { if (inb) print "@@"; open = inb = 0; next }
                 open = 1; inb = (substr($0, 4) == want); next }
        inb { print }' "$GUIDE"
}

echo "== the guide's commands, from $WORK"
blocks sh > sh.txt
while IFS= read -r line; do
    case $line in
        @@|'') continue ;;
        curl\ *|export\ PATH=*|git\ clone\ *|make\ *|cd\ contiki-ng*)
            skip "$line   (network or toolchain)"; continue ;;
        "cngsim run hello-world.sky"*)
            skip "$line   (runs what the make above built)"; continue ;;
    esac
    n=$((n + 1))
    out="cmd$n.out"
    case $line in
        *--ui*)
            # As typed, then headless: no browser, no pause, as fast as it goes.
            # exec, so that $! is cngsim itself: dash (Ubuntu's sh) does not
            # exec a -c command, and the background sh ignores SIGINT.
            sh -c "exec ${line%%#*} --no-open --start --speed max" > "$out" 2>&1 &
            pid=$!
            got=0
            for i in $(seq 1 100); do
                if curl -s http://127.0.0.1:8080/ > "page$n.html" 2>/dev/null &&
                   grep -q '<title>Cooja-NG Simulator</title>' "page$n.html"; then got=1; break; fi
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.2
            done
            # Ctrl-C ends the run (the guide says so); give it 10 s, then kill
            # it rather than wait for ever.
            kill -INT "$pid" 2>/dev/null
            ended=0
            for i in $(seq 1 50); do
                kill -0 "$pid" 2>/dev/null || { ended=1; break; }
                sleep 0.2
            done
            [ "$ended" = 1 ] || kill -KILL "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            if [ "$got" != 1 ]; then
                bad "$line   (no web UI at http://127.0.0.1:8080/)"; sed 's/^/        /' "$out" | tail -15
            elif [ "$ended" != 1 ]; then
                bad "$line   (still running 10 s after Ctrl-C)"; sed 's/^/        /' "$out" | tail -15
            else ok "$line   (served the web UI at :8080, ended on Ctrl-C)"; fi
            ;;
        *)
            if sh -c "$line" < /dev/null > "$out" 2>&1; then ok "$line"
            else bad "$line   (exit $?)"; sed 's/^/        /' "$out" | tail -15; fi
            case $line in "cngsim run rpl-udp-sky") cp "$out" run.out ;; esac
            ;;
    esac
done < sh.txt

echo "== the guide's transcripts"
blocks "" > shown.txt
awk 'BEGIN { k = 0 } /^@@$/ { k++; next } { print > ("shown" k ".txt") }' shown.txt
"$PKG/bin/cngsim" boards > boards.out 2>&1
for f in shown[0-9]*.txt; do
    [ -f "$f" ] || continue
    first=$(head -1 "$f")
    case $first in
        "cooja-ng "*) real=run.out; what="cngsim run rpl-udp-sky" ;;
        "board "*)    real=boards.out; what="cngsim boards" ;;
        *) continue ;;
    esac
    [ -f "$real" ] || { bad "$what: no output to compare (did it run?)"; continue; }
    miss=0
    while IFS= read -r want; do
        case $want in
            *...) continue ;;                       # an elision: "    ..."
            "PASSED · "*|"FAILED"*)                  # up to its wall-clock part
                stem=${want%% in *}
                grep -qF -- "$stem in " "$real" || { bad "$what: no line starting '$stem in'"; miss=1; } ;;
            *)
                grep -qxF -- "$want" "$real" || { bad "$what: shows '$want', which it does not print"; miss=1; } ;;
        esac
    done < "$f"
    [ "$miss" = 0 ] && ok "$what: every line the guide shows ($(grep -vc '\.\.\.$' "$f"))"
done

echo "== the guide's check and help mentions"
grep -oE '`cngsim (check|help)[^`]*`' "$GUIDE" | tr -d '`' | sort -u > mentions.txt
while IFS= read -r line; do
    if sh -c "$line" < /dev/null > mention.out 2>&1; then ok "$line"
    else bad "$line   (exit $?)"; sed 's/^/        /' mention.out | tail -10; fi
done < mentions.txt

if [ "$fails" -gt 0 ]; then echo "quick start: $fails FAILED"; exit 1; fi
echo "quick start: OK ($n commands run)"
