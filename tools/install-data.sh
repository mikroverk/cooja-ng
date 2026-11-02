#!/bin/sh
# install-data.sh DEST — copy what cngsim needs besides its binary into DEST:
#
#   DEST/examples/   the commented example simulations
#   DEST/firmware/   the firmware they and the templates run (what git
#                    tracks: no local .cooja builds, no Zephyr scratch)
#   DEST/tools/      the Contiki-NG suite runner and the .csc converter
#   DEST/docs/       the user guides, CHANGELOG.md and LICENSE
#   DEST/configs/, DEST/test/scripts/
#                    the setups and shell scripts those guides run
#
# DEST is cngsim's data directory: <prefix>/share/cooja-ng for `make
# install`, the package root for a release tarball.  Run from anywhere.
set -eu
[ $# -eq 1 ] || { echo "usage: $0 DEST" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=$1
mkdir -p "$DEST/examples" "$DEST/firmware" "$DEST/tools" "$DEST/docs"

cp "$ROOT"/examples/*.yaml "$DEST/examples/"

# The tracked firmware: git knows which files those are.  Outside a git
# checkout (an unpacked source tarball), everything but the two local-only
# directories.
cd "$ROOT"
if git -C "$ROOT" rev-parse --git-dir > /dev/null 2>&1; then
    git -C "$ROOT" ls-files firmware
else
    find firmware -type f ! -path 'firmware/cooja/*' ! -path 'firmware/zephyr/*'
fi | while IFS= read -r f; do
    mkdir -p "$DEST/$(dirname "$f")"
    cp "$f" "$DEST/$f"
done

for t in run-cooja-tests.sh csc2json.py build-test-firmware.sh test-border-router.sh; do
    cp "tools/$t" "$DEST/tools/"
done
mkdir -p "$DEST/docs/design"
for d in quick-start.md shell.md test-format.md ui-replay.md contiki-ng-testing.md \
         zephyr.md riot.md openthread.md design/contiki-ng-testing-plan.md; do
    cp "docs/$d" "$DEST/docs/$d"
done
cp CHANGELOG.md LICENSE "$DEST/docs/"

# Every relative link in the installed guides must land on an installed
# file: a guide added to the list above can bring links to ones that are not.
# (CHANGELOG.md is the repository's record; its links name the repository.)
dangling=0
for doc in $(cd "$DEST/docs" && find . -name '*.md' ! -name CHANGELOG.md); do
    dir=$(dirname "$doc")
    for link in $(grep -o '](\([^)#]*\)' "$DEST/docs/$doc" | sed 's/^](//' | grep -v -e '^[a-z]*:' -e '^$'); do
        if [ ! -e "$DEST/docs/$dir/$link" ]; then
            echo "install-data.sh: docs/$doc links to $link, which is not installed" >&2
            dangling=1
        fi
    done
done

# The setups and shell scripts the installed guides run (`test_runner test
# configs/...`), so their commands work from the package root as they do
# from a checkout.  A name that is no file is a placeholder for the
# reader's own (configs/my-test.json).
for f in $(cd "$DEST/docs" && find . -name '*.md' ! -name CHANGELOG.md -exec cat {} + |
           grep -oE '[A-Za-z0-9_./-]*(configs|test/scripts)/[A-Za-z0-9._-]+\.(json|yaml|cnsh)' |
           grep -E '^(configs|test/scripts)/' | sort -u); do
    [ -f "$f" ] || continue
    mkdir -p "$DEST/$(dirname "$f")"
    cp "$f" "$DEST/$f"
done
[ "$dangling" -eq 0 ]
