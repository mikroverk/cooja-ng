#!/bin/sh
# install-data.sh DEST — copy what cngsim needs besides its binary into DEST:
#
#   DEST/examples/   the commented example simulations
#   DEST/firmware/   the firmware they and the templates run (what git
#                    tracks: no local .cooja builds, no Zephyr scratch)
#   DEST/tools/      the Contiki-NG suite runner and the .csc converter
#   DEST/docs/       the user guides, CHANGELOG.md and LICENSE
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
for d in quick-start.md shell.md test-format.md ui-replay.md contiki-ng-testing.md; do
    cp "docs/$d" "$DEST/docs/"
done
cp CHANGELOG.md LICENSE "$DEST/docs/"
