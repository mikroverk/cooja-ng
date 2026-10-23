#!/bin/sh
# Install and activate the Emscripten SDK that experiments/browser-wasm is
# built and checked with, into $1 (default: $RUNNER_TEMP/emsdk).  Used by both
# test.yml (the WASM build and parity on every PR) and pages.yml (the build
# that is published) so the two cannot drift.  Afterwards:
#   . "$1/emsdk_env.sh"
#
# Pinned: the parity check compares the WASM build's output with native byte
# for byte, and a new emcc brings new warnings under -Werror; a version change
# should be a deliberate one.
set -eu
VER=6.0.13
DIR=${1:-$RUNNER_TEMP/emsdk}
[ -d "$DIR" ] || git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$DIR"
"$DIR/emsdk" install "$VER"
"$DIR/emsdk" activate "$VER"
