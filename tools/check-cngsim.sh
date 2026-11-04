#!/bin/sh
# check-cngsim.sh — cngsim end to end, from outside the checkout.
#
#   1. One simulation path: for every example and a set of configs, the node
#      console lines and the exit code of `cngsim run X` equal those of
#      `test_runner test X` (cngsim only changes what reaches the terminal).
#   2. The examples run by name from another directory and pass.
#   3. `cngsim new` writes a setup for each template that `cngsim check`
#      accepts and that runs and passes -- up to 16 nodes in every layout,
#      still after it is moved, and with every default taken on a terminal.
#   4. Bad input fails with exit 2 and says why: an unknown command, an
#      unknown example, an image with no board, a duration without a unit,
#      checks that name a node the setup does not have.
#   5. Ctrl-C ends a run with exit 130 and INTERRUPTED, not a failed check.
#   6. The README's platform table matches the board registry: every board
#      `cngsim boards` lists has a row, and every extension a row names loads.
#   7. Zephyr, RIOT and older Zephyr board names load as their board, a near
#      miss is refused with the names it may have meant, and a west build's
#      .config or RIOT's bin/<BOARD>/ picks the board without --board.
#
# Usage: tools/check-cngsim.sh   (after make; run from the repo root)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CNGSIM="$ROOT/build/cngsim"
RUNNER="$ROOT/build/test_runner"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/check-cngsim.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fails=0

ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }
console() { grep -E '^ +[0-9]+\.[0-9]{3} \[Node ' || true; }
# What cngsim changes on a console line, done to test_runner's lines too:
# the label becomes the node number alone (the runner's "[Node 2/ARM]",
# cngsim's "[Node 2/nrf52840-dk Zephyr]"), and ANSI escapes are rendered as
# cngsim's render_ansi does (cursor left/right, erase to the end, a line of
# nothing but escapes dropped).
plain() {
    perl -ne '
        chomp;
        next unless /^(\s*\S+ \[Node (\d+))\/[^\]]*\](.*)$/s;
        my ($head, $t) = ("$1]", $3);
        my ($had, $cur, $len, @b) = (0, 0, 0);
        while (length $t) {
            if ($t =~ s/^\e\[([^\x40-\x7e]*)([\x40-\x7e])//) {
                my ($p, $f) = ($1, $2); $had = 1;
                (my $d = $p) =~ s/\D//g; my $n = length($d) ? ($d > 4096 ? 4096 : $d + 0) : 1;
                if ($f eq "D") { $cur = $n >= $cur ? 0 : $cur - $n }
                elsif ($f eq "C") { $cur += $n; $cur = $len if $cur > $len }
                elsif (($f eq "J" || $f eq "K") && !length($d)) { $len = $cur }
            } elsif ($t =~ s/^\e\[.*//s || $t =~ s/^\e.?//s) { $had = 1 }
            elsif ($t =~ s/^\x08//) { $had = 1; $cur-- if $cur }
            else { $t =~ s/^(.)//s; $b[$cur++] = $1; $len = $cur if $cur > $len }
        }
        my $r = join "", @b[0 .. $len - 1];
        next if $had && ($r eq "" || $r eq " ");
        print "$head$r\n";'
}
# Examples slow in wall time run only their first CAP_MS here.
# thread-overlay simulates 600 s of six OpenThread nodes, whose main loops
# never sleep: about 15 min of wall time a run on an M-series Mac, more on
# CI, and these steps ran it three times.  Its partition and merge are what
# the site's demo recording checks (tools/record-demos.sh, 240 s, on every
# deploy); here the two binaries only need to agree and the setup to run.
# (Sub-GHz TSCH simulates 400 s too, but at ~280x real time: it runs whole.)
SLOW="thread-overlay" CAP_MS=60000
cap_args() {   # file, what to print for a slow one
    case " $SLOW " in *" $(basename "$1" .yaml) "*) echo "$2" ;; esac
}

echo "== one simulation path (console lines and exit code, cngsim vs test_runner)"
for cfg in "$ROOT"/examples/*.yaml configs/test-rpl-udp-sky.json configs/chain-4node-sky.yaml \
           configs/test-mixed-platform-rpl.yaml configs/test-rpl-udp-cc1352r1-launchpad.yaml \
           configs/cross-level-demo.json; do
    case "$cfg" in *shell-*) continue ;; esac
    name=$(basename "$cfg")
    capr=$(cap_args "$cfg" "-t $CAP_MS") capc=$(cap_args "$cfg" "--duration ${CAP_MS}ms")
    [ -n "$capr" ] && name="$name (first ${CAP_MS} ms)"
    (cd "$ROOT" && "$RUNNER" test "$cfg" $capr 2>/dev/null) > "$WORK/a.out"; ra=$?
    (cd "$ROOT" && "$CNGSIM" run "$cfg" $capc 2>/dev/null) > "$WORK/b.out"; rb=$?
    console < "$WORK/a.out" | plain > "$WORK/a.con"
    console < "$WORK/b.out" | plain > "$WORK/b.con"
    if [ "$ra" != "$rb" ]; then
        bad "$name: exit $rb, test_runner exits $ra"
    elif ! cmp -s "$WORK/a.con" "$WORK/b.con"; then
        bad "$name: console lines differ ($(wc -l < "$WORK/a.con") vs $(wc -l < "$WORK/b.con"))"
    elif [ ! -s "$WORK/a.con" ]; then
        bad "$name: no console lines at all"
    else
        ok "$name ($(wc -l < "$WORK/a.con" | tr -d ' ') lines, exit $ra)"
    fi
done

echo "== examples by name, from another directory"
cd "$WORK" || exit 1
for f in "$ROOT"/examples/*.yaml; do
    name=$(basename "$f" .yaml)
    case "$name" in shell-*) continue ;; esac
    cap=$(cap_args "$f" "--duration ${CAP_MS}ms")
    out=$("$CNGSIM" run "$name" $cap -q 2>&1); rc=$?
    [ -n "$cap" ] && out="$out (the first ${CAP_MS} ms)"
    case "$rc:$out" in
        0:PASSED*|0:DONE*) ok "$name: $out" ;;
        *) bad "$name: exit $rc: $out" ;;
    esac
done
cp "$ROOT/examples/hello-sky.yaml" "$WORK/copied.yaml"
out=$("$CNGSIM" run "$WORK/copied.yaml" -q 2>&1)
case "$out" in *"bundled firmware"*PASSED*) ok "a copied example finds the bundled firmware" ;;
    *) bad "a copied example finds the bundled firmware: $out" ;; esac
"$CNGSIM" run shell-nrf54l15 --script "$ROOT/test/scripts/shell-nrf54l15-cmd.cnsh" >/dev/null 2>&1 \
    && ok "shell-nrf54l15 with a script" || bad "shell-nrf54l15 with a script"

echo "== cngsim new -> check -> run"
# A ring of 2 or 3 without a server used to put neighbours out of range.
for spec in "rpl-udp sky 5 grid" "rpl-udp nrf52840-dk 3 line" "rpl-udp nrf52840dk 3 line" "nullnet sky 4 circle" \
            "nullnet sky 2 circle" "nullnet sky 3 circle" "hello cc2538dk 2 random" \
            "hello sky 1 grid" "thread cc2538dk 3 line" "zephyr-echo nrf52840dk 2 line" \
            "zephyr-echo cc1352r1-launchpad 2 grid" "riot-rpl nrf52840dk 5 line"; do
    set -- $spec
    f="$WORK/new/$1-$2-$3-$4.yaml"
    mkdir -p "$WORK/new"
    if ! "$CNGSIM" new "$1" --board "$2" -n "$3" --layout "$4" -o "$f" >/dev/null 2>&1; then
        bad "new $spec"; continue
    fi
    if ! "$CNGSIM" check "$f" >/dev/null 2>&1; then
        bad "check $(basename "$f")"; continue
    fi
    out=$("$CNGSIM" run "$f" -q 2>&1) && ok "new $spec: $out" || bad "new $spec: $out"
done

echo "== cngsim new: layouts that reach node 1, a moved setup, the defaults"
# From 11 nodes up the circle's radius used to exceed the radio range.
for spec in "12 circle" "16 line" "16 random" "16 grid"; do
    set -- $spec
    f="$WORK/new/big-$2.yaml"
    "$CNGSIM" new rpl-udp -n "$1" --layout "$2" -o "$f" >/dev/null 2>&1 \
        && out=$("$CNGSIM" run "$f" -q 2>&1) && ok "new rpl-udp -n $1 $2: $out" \
        || bad "new rpl-udp -n $1 $2: ${out:-not written}"
done
mkdir -p "$WORK/elsewhere"
mv "$WORK/new/big-circle.yaml" "$WORK/elsewhere/"
(cd / && "$CNGSIM" run "$WORK/elsewhere/big-circle.yaml" -q >/dev/null 2>&1) \
    && ok "a setup from new runs after it is moved" || bad "a setup from new runs after it is moved"
# A saved setup keeps the user's own paths relative: to the file it writes,
# however the config was named (./x.yaml used to save absolute paths).
mkdir -p "$WORK/own/fw" "$WORK/own2"
cp "$ROOT/firmware/sky/hello-world.sky" "$WORK/own/fw/"
printf 'version: 2\ntimeout_ms: 1000\nmote_types:\n  - { name: a, firmware: fw/hello-world.sky }\nnodes:\n  - { type: a, id: 1 }\n' \
    > "$WORK/own/x.yaml"
(cd "$WORK/own" && "$CNGSIM" run ./x.yaml --save-config out.yaml -q >/dev/null 2>&1 \
    && "$CNGSIM" run x.yaml --save-config ../own2/out.yaml -q >/dev/null 2>&1)
if grep -q 'firmware: fw/hello-world.sky' "$WORK/own/out.yaml" 2>/dev/null &&
   grep -q 'firmware: ../own/fw/hello-world.sky' "$WORK/own2/out.yaml" 2>/dev/null &&
   (cd / && "$CNGSIM" run "$WORK/own2/out.yaml" -q >/dev/null 2>&1); then
    ok "--save-config writes the setup's own paths relative to the saved file"
else
    bad "--save-config writes the setup's own paths relative to the saved file"
fi
# Enter at every prompt takes the bracketed defaults (the default used to be
# wiped by the answer it shared a buffer with).
python3 - "$CNGSIM" "$WORK/tty" <<'PY' && ok "new on a terminal: Enter takes the defaults" \
    || bad "new on a terminal: Enter takes the defaults"
import os, pty, sys, time
os.makedirs(sys.argv[2], exist_ok=True)
pid, fd = pty.fork()
if pid == 0:
    os.chdir(sys.argv[2])
    os.execv(sys.argv[1], [sys.argv[1], 'new'])
for _ in range(5):
    time.sleep(0.3)
    os.write(fd, b'\n')
try:
    while os.read(fd, 4096):
        pass
except OSError:
    pass
_, st = os.waitpid(pid, 0)
sys.exit(0 if st == 0 and os.path.exists(os.path.join(sys.argv[2], 'sim.yaml')) else 1)
PY

echo "== bad input exits 2 and says why"
expect2() {   # label, command...
    label=$1; shift
    err=$("$@" 2>&1 >/dev/null); rc=$?
    if [ "$rc" = 2 ] && [ -n "$err" ]; then ok "$label"; else bad "$label (exit $rc, '$err')"; fi
}
expect2 "unknown command" "$CNGSIM" frobnicate
expect2 "unknown example" "$CNGSIM" run no-such-example
cp "$ROOT/firmware/sky/hello-world.sky" "$WORK/hello.elf"
printf 'version: 2\nmote_types:\n  - { name: a, firmware: hello.elf }\nnodes:\n  - { type: a, id: 1 }\n' \
    > "$WORK/noboard.yaml"
expect2 "an .elf with no board:" "$CNGSIM" run "$WORK/noboard.yaml"
expect2 "test_runner: unknown mode" "$RUNNER" frobnicate
sed 's/name: a,/name: a, board: sky,/' "$WORK/noboard.yaml" > "$WORK/board.yaml"
"$CNGSIM" run "$WORK/board.yaml" --duration 1s -q >/dev/null 2>&1 \
    && ok "the same .elf with board: sky runs" || bad "the same .elf with board: sky runs"
expect2 "an .elf on the command line with no --board" "$CNGSIM" run "$WORK/hello.elf" --duration 1s
"$CNGSIM" run "$WORK/hello.elf" --board sky --duration 1s -q >/dev/null 2>&1 \
    && ok "the .elf with --board sky runs" || bad "the .elf with --board sky runs"
cp "$ROOT/firmware/nrf52840-dongle/hello-world.nrf52840-dongle" "$WORK/hello.nrf52840"
printf 'version: 2\nmote_types:\n  - { name: a, board: nrf52840-dongle, firmware: hello.nrf52840 }\nnodes:\n  - { type: a, id: 1 }\n' \
    > "$WORK/dongle.yaml"
"$CNGSIM" run "$WORK/dongle.yaml" --duration 1s -q >/dev/null 2>&1 \
    && ok "board: nrf52840-dongle on a .nrf52840 image" || bad "board: nrf52840-dongle on a .nrf52840 image"
expect2 "a duration without a unit" "$CNGSIM" run hello-sky -t 10
expect2 "a duration longer than a run can simulate (720h)" "$CNGSIM" run hello-sky --duration 720h
expect2 "a duration under 1 ms" "$CNGSIM" run hello-sky --duration 0.0004s
expect2 "test_runner -t beyond an int" "$RUNNER" multinode "$ROOT/firmware/sky/hello-world.sky" -t 3000000000
expect2 "test_runner -t that is no number" "$RUNNER" multinode "$ROOT/firmware/sky/hello-world.sky" -t 12x
expect2 "rpl-udp with more nodes than the server answers" "$CNGSIM" new rpl-udp -n 17 -o "$WORK/x.yaml"
expect2 "nullnet with one node (nothing to receive from)" "$CNGSIM" new nullnet -n 1 -o "$WORK/x.yaml"
expect2 "zephyr-echo with three nodes (the samples' addresses are fixed)" "$CNGSIM" new zephyr-echo --board nrf52840-dk -n 3 -o "$WORK/x.yaml"
expect2 "thread on a board without an OpenThread image" "$CNGSIM" new thread --board sky -n 2 -o "$WORK/x.yaml"
sed -e 's/node: 2 }/node: 77 }/' -e "s#\.\./firmware/#$ROOT/firmware/#g" \
    "$ROOT/examples/grid-sky.yaml" > "$WORK/n77.yaml"
expect2 "check: a step names a node the setup does not have" "$CNGSIM" check "$WORK/n77.yaml"
printf '{ "nodes": [ { "firmware": "%s" }, { "firmware": "%s/missing.sky" } ],\n  "mote_types": [ { "firmware": "%s" } ] }\n' \
    "$ROOT/firmware/sky/hello-world.sky" "$WORK" "$ROOT/firmware/sky/hello-world.sky" > "$WORK/v1.json"
expect2 "check: a v1 setup with mote_types and a missing image" "$CNGSIM" check "$WORK/v1.json"

echo "== board names from Zephyr (west build -b), RIOT (BOARD=) and older Zephyr"
# Each name a system gives a board, as  board:  on a mote type, loads as
# that board; a near miss is refused with the names it may have meant.
cp "$ROOT/firmware/nrf52840-dk/zephyr-hello-world.nrf52840-dk" "$WORK/zephyr.elf"
for pair in nrf52840dk/nrf52840=nrf52840-dk nrf52840dk=nrf52840-dk \
            nrf52840dk_nrf52840=nrf52840-dk nrf52840dongle/nrf52840=nrf52840-dongle \
            nrf52840dongle=nrf52840-dongle nrf54l15dk/nrf54l15/cpuapp=nrf54l15-dk \
            nrf54l15dk=nrf54l15-dk xiao_nrf54l15/nrf54l15/cpuapp=nrf54l15-xiao \
            cc1352r1_launchxl=cc1352r1-launchpad cc1352r1_launchxl/cc1352r=cc1352r1-launchpad \
            cc1352-launchpad=cc1352r1-launchpad cc1312r7_lp=cc1312r7-launchpad \
            cc1312r7_lp/cc1352r7=cc1312r7-launchpad firefly=zoul-firefly telosb=sky \
            z1=z1 cc2538dk=cc2538dk; do
    name=${pair%=*} want=${pair#*=}
    printf 'version: 2\nmote_types:\n  - { name: a, board: "%s", firmware: zephyr.elf }\nnodes:\n  - { type: a, id: 1 }\n' \
        "$name" > "$WORK/alias.yaml"
    got=$("$CNGSIM" check "$WORK/alias.yaml" 2>&1 | tail -1 | awk '{ print $2 }')
    [ "$got" = "$want" ] && ok "board: $name is $want" || bad "board: $name is $want (got '$got')"
done
"$CNGSIM" run "$WORK/zephyr.elf" --board nrf52840dk/nrf52840 --duration 1s -q >/dev/null 2>&1 \
    && ok "a west build's zephyr.elf with --board nrf52840dk/nrf52840 runs" \
    || bad "a west build's zephyr.elf with --board nrf52840dk/nrf52840 runs"
for near in "nrf52840:nrf52840-dongle, nrf52840-dk" "nRF52840-DK:nrf52840-dk" "skyy:sky" \
            "cc1312-launchpad:cc1352r1-launchpad, cc1312r7-launchpad"; do
    name=${near%%:*} want=${near#*:}
    err=$("$CNGSIM" run "$WORK/zephyr.elf" --board "$name" --duration 1s 2>&1 >/dev/null); rc=$?
    case "$rc:$err" in
        "2:"*"did you mean $want?"*) ok "--board $name: exit 2, did you mean $want?" ;;
        *) bad "--board $name: exit $rc, '$err'" ;;
    esac
done
expect2 "--board with nothing close" "$CNGSIM" run "$WORK/zephyr.elf" --board xyz --duration 1s
err=$("$RUNNER" mixed-multinode "$WORK/zephyr.elf" --board nrf52840 -t 1000 2>&1 >/dev/null); rc=$?
case "$rc:$err" in 2:*"did you mean"*) ok "test_runner --board nrf52840: exit 2 with the names" ;;
    *) bad "test_runner --board nrf52840: exit $rc, '$err'" ;; esac
# Without --board: a west build's .config names the board, as does RIOT's
# bin/<BOARD>/ directory; a target that is not a board here is refused.
mkdir -p "$WORK/west/build/zephyr" "$WORK/riot/bin/nrf52840dk"
cp "$WORK/zephyr.elf" "$WORK/west/build/zephyr/zephyr.elf"
printf 'CONFIG_BOARD="nrf52840dk"\nCONFIG_BOARD_TARGET="nrf52840dk/nrf52840"\n' \
    > "$WORK/west/build/zephyr/.config"
out=$("$CNGSIM" run "$WORK/west/build/zephyr/zephyr.elf" --duration 1s 2>&1); rc=$?
case "$rc:$out" in 0:*"runs as nrf52840-dk"*"Hello World!"*) ok "zephyr.elf: the board from build/zephyr/.config" ;;
    *) bad "zephyr.elf: the board from build/zephyr/.config: exit $rc" ;; esac
printf 'CONFIG_BOARD="nrf52840dk"\nCONFIG_BOARD_TARGET="nrf52840dk/nrf52811"\n' \
    > "$WORK/west/build/zephyr/.config"
expect2 "zephyr.elf whose .config names nrf52840dk/nrf52811" \
    "$CNGSIM" run "$WORK/west/build/zephyr/zephyr.elf" --duration 1s
cp "$ROOT/firmware/nrf52840-dk/riot-gnrc-networking.nrf52840-dk" "$WORK/riot/bin/nrf52840dk/gnrc_networking.elf"
out=$("$CNGSIM" run "$WORK/riot/bin/nrf52840dk/gnrc_networking.elf" --duration 1s 2>&1); rc=$?
case "$rc:$out" in 0:*"runs as nrf52840-dk"*"This is RIOT"*) ok "RIOT bin/nrf52840dk/*.elf: the board from its directory" ;;
    *) bad "RIOT bin/nrf52840dk/*.elf: the board from its directory: exit $rc" ;; esac
"$CNGSIM" boards --os > "$WORK/boards-os.out" 2>&1 && grep -q '^nrf52840-dk  *nrf52840dk/nrf52840  *nrf52840dk  *ot-nrf528xx' "$WORK/boards-os.out" \
    && ok "boards --os: the nRF52840 DK's Zephyr, RIOT and OpenThread names" \
    || bad "boards --os: the nRF52840 DK's Zephyr, RIOT and OpenThread names"

echo "== Ctrl-C is an interruption, not a failed check"
"$CNGSIM" run rpl-udp-sky --duration 3000s --speed 1 > "$WORK/int.out" 2>&1 &
pid=$!
sleep 3
kill -INT "$pid"
wait "$pid"; rc=$?
if [ "$rc" = 130 ] && grep -q '^INTERRUPTED' "$WORK/int.out"; then
    ok "exit 130: $(tail -1 "$WORK/int.out")"
else
    bad "Ctrl-C: exit $rc, '$(tail -1 "$WORK/int.out")'"
fi
# -v shows the simulator's own output unfiltered; it still says INTERRUPTED.
"$CNGSIM" run rpl-udp-sky -v --duration 3000s --speed 1 > "$WORK/intv.out" 2>&1 &
pid=$!
sleep 3
kill -INT "$pid"
wait "$pid"; rc=$?
if [ "$rc" = 130 ] && grep -q '^INTERRUPTED' "$WORK/intv.out"; then
    ok "-v: exit 130: $(grep '^INTERRUPTED' "$WORK/intv.out")"
else
    bad "-v Ctrl-C: exit $rc, '$(tail -1 "$WORK/intv.out")'"
fi

echo "== README platform table vs cngsim boards"
"$CNGSIM" boards > "$WORK/boards.out" 2>&1
awk '/^## Supported platforms/ { t = 1; next } /^## / { t = 0 } t && /^\| `\./' "$ROOT/README.md" \
    > "$WORK/readme-rows.txt"
# Each extension a row names must load, as the board `check` reports for it;
# together they must cover every board in the registry.
: > "$WORK/covered.txt"
for ext in $(grep -oE '`\.[a-z0-9-]+`' "$WORK/readme-rows.txt" | tr -d '`.' | sort -u); do
    : > "$WORK/image.$ext"
    printf '{ "version": 2, "mote_types": [ { "name": "t", "firmware": "%s" } ], "nodes": [ { "type": "t" } ] }\n' \
        "$WORK/image.$ext" > "$WORK/ext.json"
    if "$CNGSIM" check "$WORK/ext.json" > "$WORK/ext.out" 2>&1; then
        board=$(tail -1 "$WORK/ext.out" | awk '{ print $2 }')
        echo "$board" >> "$WORK/covered.txt"
        ok "README .$ext loads as $board"
    else
        bad "README .$ext: $(head -1 "$WORK/ext.out")"
    fi
done
for b in $(awk 'NR > 1 && NF == 0 { exit } NR > 1 { print $1 }' "$WORK/boards.out"); do
    grep -qxF "$b" "$WORK/covered.txt" || bad "board $b (cngsim boards) has no README row"
done

echo
if [ "$fails" -eq 0 ]; then echo "check-cngsim: OK"; else echo "check-cngsim: $fails failed"; fi
[ "$fails" -eq 0 ]
