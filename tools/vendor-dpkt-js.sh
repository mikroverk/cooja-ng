#!/usr/bin/env bash
#
# Put dpkt-js (https://github.com/mikroverk/dpkt-js) into the web UI page.
#
#   tools/vendor-dpkt-js.sh [VERSION]      # default: the version the page has now
#
# The web UI is one file served four ways (built into the binary, --ui PATH,
# the site's player.html, the WASM player), so the dissector goes inside it:
# the npm release, bundled by esbuild into one minified script that sets
# window.dpktjs, between the dpkt-js markers in ui/index.html.  Run it to
# update dpkt-js; the result is committed, the build never fetches anything.
# Needs node and network access.
set -euo pipefail
cd "$(dirname "$0")/.."

PAGE=ui/index.html
ESBUILD=0.25.0
VERSION=${1:-$(sed -n 's|.*dpkt-js \([0-9][0-9.]*\) .*vendored by tools/vendor-dpkt-js.sh.*|\1|p' "$PAGE" | head -1)}
[ -n "$VERSION" ] || { echo "vendor-dpkt-js: give a version (no dpkt-js block in $PAGE yet)" >&2; exit 2; }
grep -q '<!-- dpkt-js:begin -->' "$PAGE" && grep -q '<!-- dpkt-js:end -->' "$PAGE" ||
    { echo "vendor-dpkt-js: $PAGE has no <!-- dpkt-js:begin/end --> markers" >&2; exit 2; }

W=$(mktemp -d "${TMPDIR:-/tmp}/vendor-dpkt-js.XXXXXX")
trap 'rm -rf "$W"' EXIT
(cd "$W" && echo '{"private":true}' > package.json &&
 npm install --no-audit --no-fund --silent "dpkt-js@$VERSION" "esbuild@$ESBUILD" >/dev/null)
INTEGRITY=$(node -p "require('$W/node_modules/dpkt-js/package.json').version + ' ' + (require('$W/package-lock.json').packages['node_modules/dpkt-js'].integrity)")
"$W/node_modules/.bin/esbuild" "$W/node_modules/dpkt-js/dist/index.js" --bundle --format=iife \
    --global-name=dpktjs --minify --target=es2020 --legal-comments=none --log-level=warning \
    --outfile="$W/dpkt.js"

{
    echo '<!-- dpkt-js:begin -->'
    echo "<script>/* dpkt-js $VERSION (BSD-3-Clause, github.com/mikroverk/dpkt-js), vendored by tools/vendor-dpkt-js.sh"
    echo "   from npm (${INTEGRITY#* }), bundled with esbuild $ESBUILD.  Do not edit: rerun the script. */"
    cat "$W/dpkt.js"
    echo '</script>'
    echo '<!-- dpkt-js:end -->'
} > "$W/block.html"

# Replace everything between the markers (inclusive) with the new block.
awk -v block="$W/block.html" '
    /<!-- dpkt-js:begin -->/ { while ((getline line < block) > 0) print line; skip = 1; next }
    /<!-- dpkt-js:end -->/   { skip = 0; next }
    !skip
' "$PAGE" > "$W/page.html"
mv "$W/page.html" "$PAGE"
echo "vendor-dpkt-js: dpkt-js ${INTEGRITY%% *} in $PAGE ($(wc -c < "$W/dpkt.js" | tr -d ' ') bytes)"
