#!/bin/sh
#
# build.sh -- assemble the WebAssembly site into build/web.
#
#   ./web/build.sh /path/to/dosbox-x-wasm
#
# Expects a DOSBox-X tree already built with build-emscripten-sdl2 and with
# integration/dosbox-x/apply.sh applied. Copies the loader and binary, and
# assembles the DOS test programs next to the pages.

set -eu

WASMTREE=${1:-}
if [ -z "$WASMTREE" ] || [ ! -f "$WASMTREE/src/dosbox-x.wasm" ]; then
    echo "usage: $0 /path/to/dosbox-x-wasm   (with src/dosbox-x.wasm built)" >&2
    exit 2
fi

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
OUT="$ROOT/build/web"

mkdir -p "$OUT"
cp "$WASMTREE/src/dosbox-x"      "$OUT/dosbox-x.js"
cp "$WASMTREE/src/dosbox-x.wasm" "$OUT/dosbox-x.wasm"
cp "$HERE"/*.html                "$OUT/"
nasm -f bin -o "$OUT/BENCH.COM" "$ROOT/tests/dos/bench.asm"
nasm -f bin -o "$OUT/INT33.COM" "$ROOT/tests/dos/int33probe.asm"

ls -la "$OUT"
