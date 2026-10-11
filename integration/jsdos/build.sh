#!/bin/bash
#
# build.sh -- build js-dos's classic DOSBox with the VFX1 device, into
# build/web/emulators/ where web/vfx1.html loads it from.
#
#   bash integration/jsdos/build.sh [/path/to/js-dos/emulators]
#
# Run inside Linux (WSL on Windows). Needs emsdk 5.0.2 at $EMSDK_DIR (default
# ~/emsdk5): that is what js-dos's CI builds 8.4.1 with, despite its Dockerfile
# naming 3.1.28, and 3.1.28's JS optimizer rejects the worker's JavaScript.
# Also cmake >= 3.21 and ninja on PATH (or in ~/vfxtools). Clones emulators
# v8.4.1 to ~/jsdos-emu if no tree is given.
#
# Only wdosbox.js and wdosbox.wasm are built, finished the way js-dos's gulp
# build finishes them. The rest of the emulators folder
# (emulators.js, wlibzip) comes unchanged from the js-dos CDN, which must
# be the same release as the tag built here; the script checks.

set -eu

JSDOS_TAG=v8.4.1
EMU=${1:-$HOME/jsdos-emu}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
# VFX1_OPT picks the optimisation level; anything but the default builds in its
# own directory and lands in build/web/emulators<level>, for side-by-side tests.
VFX1_OPT=${VFX1_OPT:--Oz}
SUFFIX=""
[ "$VFX1_OPT" = "-Oz" ] || SUFFIX=$(printf '%s' "$VFX1_OPT" | tr -c 'A-Za-z0-9' '-')
OUT="$ROOT/build/web/emulators$SUFFIX"

EMSDK_DIR=${EMSDK_DIR:-$HOME/emsdk5}
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1
[ -d "$HOME/vfxtools" ] && export PATH="$HOME/vfxtools/cmake-3.30.5-linux-x86_64/bin:$HOME/vfxtools:$PATH"

if [ ! -d "$EMU" ]; then
    git clone -q --branch "$JSDOS_TAG" --depth 1 https://github.com/js-dos/emulators.git "$EMU"
    git -C "$EMU" -c url."https://github.com/".insteadOf="git@github.com:" \
        submodule update --init --depth 1 native/dosbox
fi

sh "$HERE/apply.sh" "$EMU"

mkdir -p "$EMU/build-wasm$SUFFIX"
cd "$EMU/build-wasm$SUFFIX"
if [ ! -f build.ninja ]; then
    emcmake cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DVFX1_OPT="$VFX1_OPT" ..
fi
ninja wdosbox

mkdir -p "$OUT"
# The CDN has no versioned paths, only latest; refuse a mismatched release.
for f in emulators.js wlibzip.js wlibzip.wasm; do
    [ -f "$OUT/$f" ] || curl -sSfL -o "$OUT/$f" "https://v8.js-dos.com/latest/emulators/$f"
done
if ! grep -q "${JSDOS_TAG#v}" "$OUT/emulators.js"; then
    echo "build.sh: emulators.js on the CDN is not ${JSDOS_TAG#v}; rebuild at the matching tag" >&2
    exit 1
fi
# js-dos's gulp step appends the worker bootstrap to the Emscripten output;
# without it the worker never starts the emulator.
python3 - "$EMU" "$OUT" <<'PYTHON'
import sys
emu, out = sys.argv[1], sys.argv[2]
js = open("wdosbox.js", encoding="utf-8").read()
js += open(f"{emu}/src/dos/dosbox/ts/worker-server.js", encoding="utf-8").read()
js = js.replace("@MODULE_NAME@", "WDOSBOX").replace("@SOCKDRIVE@", "")
open(f"{out}/wdosbox.js", "w", encoding="utf-8").write(js)
PYTHON
cp wdosbox.wasm "$OUT/"
ls -la "$OUT"
