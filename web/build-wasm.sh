#!/bin/sh
#
# build-wasm.sh -- build DOSBox-X for WebAssembly with the VFX1 overlay.
#
#   ./web/build-wasm.sh [/path/to/dosbox-x-wasm]
#
# Follows upstream's build-emscripten-sdl2 recipe, which is tested against
# Emscripten 3.1.28, with one addition that is required, not optional:
#
#   --disable-gamelink
#
# With GameLink enabled, DOSBox-X allocates emulated RAM through
# GameLink::AllocRAM, but upstream excludes the GameLink library from
# Emscripten builds. Upstream's recipe also links with
# ERROR_ON_UNDEFINED_SYMBOLS=0, so the build succeeds and the emulator aborts
# at boot with "missing function: _ZN8GameLink8AllocRAMEj". Found the hard way.
#
# The same linker setting still leaves these unresolved. Each aborts the page
# if the feature is used, so none of them may be enabled in a browser config:
#
#   serial port passthrough     SERIAL_open, SERIAL_close, ...
#   core = full, core = simple  CPU_Core_Full_Run, CPU_Core_Simple_Run
#   IBM Music Feature card      VoiceDefinition, InstrumentConfiguration
#
# Always set core = normal explicitly: it is the interpreter the build has.

set -eu

TREE=${1:-$HOME/src/dosbox-x-wasm}
EMSDK_VERSION=3.1.28
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)

if [ ! -d "$HOME/emsdk" ]; then
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$HOME/emsdk"
fi
( cd "$HOME/emsdk" && ./emsdk install "$EMSDK_VERSION" && ./emsdk activate "$EMSDK_VERSION" )

if [ ! -d "$TREE/.git" ]; then
    git clone --depth 1 https://github.com/joncampbell123/dosbox-x.git "$TREE"
fi
sh "$ROOT/integration/dosbox-x/apply.sh" "$TREE"

# shellcheck disable=SC1091
. "$HOME/emsdk/emsdk_env.sh"
cd "$TREE"
bash ./build-emscripten-sdl2 --disable-gamelink

sh "$HERE/build.sh" "$TREE"
