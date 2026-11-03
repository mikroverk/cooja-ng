#!/usr/bin/env bash
#
# Build a Contiki-NG firmware ELF for a csim device port and stamp a
# PROVENANCE.md alongside it.
#
# Usage:
#   tools/build-device-firmware.sh \
#       --target zoul \
#       --board firefly \
#       --example examples/hello-world \
#       --output firmware/zoul/hello-world.zoul-firefly
#
# Optional:
#   --contiki-dir DIR     Contiki-NG checkout (default: $CONTIKI_DIR or
#                         ../contiki-ng).
#   --local               Build with the host toolchain instead of the
#                         contiker/contiki-ng Docker image.
#   --make-args "..."     Extra arguments passed verbatim to make
#                         (e.g. "MAKE_MAC=MAKE_MAC_TSCH WERROR=0").
#   --source-file NAME    Pick a specific .c file inside the example
#                         dir (e.g. udp-client.c). Default: first .c
#                         file with a main() or PROCESS_THREAD.
#   --no-provenance       Skip writing PROVENANCE.md (rare; CI may want it
#                         off when stamping in a separate step).
#   --debug               Keep DWARF in the image (DEBUG=1, CFLAGS_DEBUG=-g),
#                         so overlays can read struct layouts straight from
#                         it.  -g changes no code: at the same commit and
#                         toolchain the program is the same as without it
#                         (check with tools/check-same-program.py).
#   --relstr STR          The version string the firmware prints
#                         ("Contiki-NG-STR").  Default: git describe of the
#                         Contiki-NG checkout, taken on the host, since git
#                         cannot read the mounted tree inside Docker (the
#                         image then printed a bare "Contiki-NG").
#   --layout HEADER       Layout rows only: rebuild the example with DWARF
#                         (DEBUG=1, CFLAGS_DEBUG=-g) and merge the struct
#                         layouts HEADER asks for into it, under --output's
#                         file name (tools/gen-struct-layout.py; needs
#                         pyelftools, $PYTHON or python3).  Neither the image
#                         nor PROVENANCE.md is written: -g changes no code,
#                         so the rows hold for the image built without it.
#                         Give the image's own build command plus --layout.
#
# Default toolchain: Docker image contiker/contiki-ng:latest. The image
# already has arm-none-eabi-gcc + msp430-gcc + the Contiki build harness,
# so this script Just Works on any host with Docker.
set -euo pipefail

usage() {
    awk '/^set -euo/{exit} NR>1{sub(/^# ?/,""); print}' "$0"
    exit "${1:-1}"
}

# ---- defaults ----
TARGET=""
BOARD=""
EXAMPLE=""
OUTPUT=""
CONTIKI_DIR="${CONTIKI_DIR:-}"
USE_DOCKER=1
EXTRA_MAKE_ARGS=""
WRITE_PROVENANCE=1
DOCKER_IMAGE="contiker/contiki-ng:latest"
# GNU make: On the host, Contiki-NG's makefiles are GNU syntax, and "make" is BSD make
# on the BSDs.
case $(uname -s) in *BSD|DragonFly) MAKE=${MAKE:-gmake} ;; *) MAKE=${MAKE:-make} ;; esac
SRC_FILE_ARG=""
LAYOUT_HEADER=""
DEBUG_IMAGE=0
RELSTR_ARG=""

# ---- args ----
while [ $# -gt 0 ]; do
    case "$1" in
        --target)         TARGET="$2"; shift 2 ;;
        --board)          BOARD="$2"; shift 2 ;;
        --example)        EXAMPLE="$2"; shift 2 ;;
        --source-file)    SRC_FILE_ARG="$2"; shift 2 ;;
        --output)         OUTPUT="$2"; shift 2 ;;
        --contiki-dir)    CONTIKI_DIR="$2"; shift 2 ;;
        --local)          USE_DOCKER=0; shift ;;
        --make-args)      EXTRA_MAKE_ARGS="$2"; shift 2 ;;
        --no-provenance)  WRITE_PROVENANCE=0; shift ;;
        --image)          DOCKER_IMAGE="$2"; shift 2 ;;
        --layout)         LAYOUT_HEADER="$2"; shift 2 ;;
        --debug)          DEBUG_IMAGE=1; shift ;;
        --relstr)         RELSTR_ARG="$2"; shift 2 ;;
        -h|--help)        usage 0 ;;
        *) echo "unknown arg: $1" >&2; usage 1 ;;
    esac
done

[ -n "$TARGET" ]  || { echo "--target required"  >&2; exit 1; }
[ -n "$EXAMPLE" ] || { echo "--example required" >&2; exit 1; }
[ -n "$OUTPUT" ]  || { echo "--output required"  >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CSIM_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---- resolve Contiki-NG ----
if [ -z "$CONTIKI_DIR" ] && [ -f "$CSIM_DIR/csim.conf" ]; then
    CONTIKI_DIR=$(grep -s '^CONTIKI_DIR=' "$CSIM_DIR/csim.conf" | cut -d= -f2-)
fi
CONTIKI_DIR="${CONTIKI_DIR:-$CSIM_DIR/../contiki-ng}"
if [ ! -d "$CONTIKI_DIR" ]; then
    echo "Error: Contiki-NG not found at $CONTIKI_DIR" >&2
    echo "Pass --contiki-dir, set CONTIKI_DIR, or edit csim.conf." >&2
    exit 1
fi
CONTIKI_DIR="$(cd "$CONTIKI_DIR" && pwd)"

# Allow --example as a relative path under Contiki, or an absolute path.
if [ -d "$CONTIKI_DIR/$EXAMPLE" ]; then
    SRC_DIR="$CONTIKI_DIR/$EXAMPLE"
elif [ -d "$EXAMPLE" ]; then
    SRC_DIR="$(cd "$EXAMPLE" && pwd)"
else
    echo "Error: example dir not found: $EXAMPLE" >&2
    exit 1
fi

# Determine the source name. If --source-file was passed, use it directly
# (with or without the .c suffix). Otherwise pick the first .c file with
# a `main`/`PROCESS_THREAD` in the source dir.
# Contiki uses `<name>.c` -> `<name>.<target>`.
SRC_NAME=""
if [ -n "$SRC_FILE_ARG" ]; then
    SRC_NAME="${SRC_FILE_ARG%.c}"
    [ -f "$SRC_DIR/$SRC_NAME.c" ] || {
        echo "Error: --source-file '$SRC_NAME.c' not found in $SRC_DIR" >&2
        exit 1
    }
else
    for f in "$SRC_DIR"/*.c; do
        [ -f "$f" ] || continue
        if grep -qE 'PROCESS_THREAD|int +main *\(' "$f"; then
            SRC_NAME="$(basename "$f" .c)"
            break
        fi
    done
fi
[ -n "$SRC_NAME" ] || { echo "Error: no main/PROCESS in $SRC_DIR/*.c" >&2; exit 1; }

USER_MAKE_ARGS="$EXTRA_MAKE_ARGS"   # what the provenance stamp records

# The version string, from the host's view of the checkout.
RELSTR="${RELSTR_ARG:-$(git -C "$CONTIKI_DIR" describe --tags --always --dirty 2>/dev/null || true)}"
[ -n "$RELSTR" ] && EXTRA_MAKE_ARGS="$EXTRA_MAKE_ARGS RELSTR=$RELSTR"
[ $DEBUG_IMAGE -eq 1 ] && EXTRA_MAKE_ARGS="$EXTRA_MAKE_ARGS DEBUG=1 CFLAGS_DEBUG=-g"

# Layout rows come from a DWARF build of the same source and configuration.
if [ -n "$LAYOUT_HEADER" ]; then
    [ -f "$LAYOUT_HEADER" ] || { echo "Error: --layout header not found: $LAYOUT_HEADER" >&2; exit 1; }
    LAYOUT_HEADER="$(cd "$(dirname "$LAYOUT_HEADER")" && pwd)/$(basename "$LAYOUT_HEADER")"
    EXTRA_MAKE_ARGS="$EXTRA_MAKE_ARGS DEBUG=1 CFLAGS_DEBUG=-g"
fi

# ---- build ----
# Create the output directory first: resolving it with cd before it exists
# fails inside the substitution and left OUTPUT_ABS as "/<name>".
mkdir -p "$(dirname "$OUTPUT")"
OUTPUT_ABS="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"

BOARD_ARG=""
[ -n "$BOARD" ] && BOARD_ARG="BOARD=$BOARD"

echo "=== build-device-firmware ==="
echo "  Contiki-NG:   $CONTIKI_DIR"
echo "  Source:       $SRC_DIR"
echo "  Source file:  $SRC_NAME.c"
echo "  TARGET:       $TARGET"
echo "  BOARD:        ${BOARD:-<default>}"
echo "  Make args:    $EXTRA_MAKE_ARGS"
echo "  Output:       $OUTPUT_ABS"
echo "  Toolchain:    $([ $USE_DOCKER -eq 1 ] && echo "Docker ($DOCKER_IMAGE)" || echo "host")"
echo ""

build_in_dir() {
    local cmd="$MAKE TARGET=$TARGET $BOARD_ARG $EXTRA_MAKE_ARGS WERROR=0 -C $SRC_DIR distclean >/dev/null 2>&1 || true; \
                $MAKE TARGET=$TARGET $BOARD_ARG $EXTRA_MAKE_ARGS WERROR=0 -j\$(nproc 2>/dev/null || sysctl -n hw.ncpu) -C $SRC_DIR $SRC_NAME.$TARGET"
    bash -c "$cmd"
}

if [ $USE_DOCKER -eq 1 ]; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "Error: docker not found. Install Docker or pass --local." >&2
        exit 1
    fi
    docker run --rm \
        -v "$CONTIKI_DIR:/home/user/contiki-ng" \
        -w "/home/user/contiki-ng/${SRC_DIR#$CONTIKI_DIR/}" \
        "$DOCKER_IMAGE" \
        bash -c "make TARGET=$TARGET $BOARD_ARG $EXTRA_MAKE_ARGS WERROR=0 distclean >/dev/null 2>&1 || true; \
                 make TARGET=$TARGET $BOARD_ARG $EXTRA_MAKE_ARGS WERROR=0 -j\$(nproc) $SRC_NAME.$TARGET"
else
    build_in_dir
fi

# Locate the produced ELF (may be in src dir or build/$TARGET/).
BUILT=""
for cand in "$SRC_DIR/$SRC_NAME.$TARGET" \
            "$SRC_DIR/build/$TARGET/${BOARD:-}/$SRC_NAME.$TARGET" \
            "$SRC_DIR/build/$TARGET/$SRC_NAME.$TARGET"; do
    [ -f "$cand" ] && { BUILT="$cand"; break; }
done
[ -n "$BUILT" ] || { echo "Error: built ELF not found near $SRC_DIR" >&2; exit 1; }

if [ -n "$LAYOUT_HEADER" ]; then
    "${PYTHON:-python3}" "$SCRIPT_DIR/gen-struct-layout.py" --update "$LAYOUT_HEADER" \
        --name "$(basename "$OUTPUT")" "$BUILT"
    exit $?
fi

cp "$BUILT" "$OUTPUT_ABS"
echo ""
echo "  -> wrote $OUTPUT_ABS ($(wc -c < "$OUTPUT_ABS" | tr -d ' ') bytes)"

# ---- provenance stamp ----
if [ $WRITE_PROVENANCE -eq 1 ]; then
    PROV_PATH="$(dirname "$OUTPUT_ABS")/PROVENANCE.md"
    COMMIT="$(git -C "$CONTIKI_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
    BUILD_CMD="tools/build-device-firmware.sh --target $TARGET${BOARD:+ --board $BOARD} --example $EXAMPLE --output $OUTPUT"
    [ -n "$SRC_FILE_ARG" ]   && BUILD_CMD="$BUILD_CMD --source-file $SRC_FILE_ARG"
    [ -n "$USER_MAKE_ARGS" ]  && BUILD_CMD="$BUILD_CMD --make-args \"$USER_MAKE_ARGS\""
    [ $DEBUG_IMAGE -eq 1 ]    && BUILD_CMD="$BUILD_CMD --debug"
    [ -n "$RELSTR_ARG" ]      && BUILD_CMD="$BUILD_CMD --relstr $RELSTR_ARG"
    [ $USE_DOCKER -eq 0 ]     && BUILD_CMD="$BUILD_CMD --local"
    DATE_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    BUILDER="$(git config user.name 2>/dev/null || whoami)"

    # Append-or-create PROVENANCE.md entry per ELF.
    {
        if [ ! -f "$PROV_PATH" ]; then
            echo "# Firmware Provenance — $(basename "$(dirname "$OUTPUT_ABS")")"
            echo ""
            echo "Generated by tools/build-device-firmware.sh. One section per ELF."
            echo ""
        fi
        echo "## $(basename "$OUTPUT_ABS")"
        echo ""
        echo "- **Source**: contiki-ng commit \`$COMMIT\`"
        echo "- **Source path**: \`${SRC_DIR#$CONTIKI_DIR/}\` (file: \`$SRC_NAME.c\`)"
        echo "- **TARGET**: \`$TARGET\`"
        [ -n "$BOARD" ] && echo "- **BOARD**: \`$BOARD\`"
        [ -n "$USER_MAKE_ARGS" ] && echo "- **Make args**: \`$USER_MAKE_ARGS\`"
        echo "- **Toolchain**: $([ $USE_DOCKER -eq 1 ] && echo "Docker $DOCKER_IMAGE" || echo "host")"
        echo "- **Built**: $DATE_UTC by $BUILDER"
        echo "- **Build command**: \`$BUILD_CMD\`"
        echo ""
    } >> "$PROV_PATH"
    echo "  -> stamped $PROV_PATH"
fi

echo ""
echo "Done."
