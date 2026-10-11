#!/bin/sh
#
# syntax-check.sh -- type-check the VFX1 sources against real DOSBox-X headers
# without a full DOSBox-X build.
#
#   ./integration/dosbox-x/syntax-check.sh /path/to/dosbox-x
#
# Why this exists: a real build needs autotools, SDL, X11 and a dozen other
# development packages. This needs only g++ and the DOSBox-X source tree, so
# the module can be checked on a machine that cannot build the emulator.
#
# What it proves: the module parses, and every DOSBox-X symbol it calls
# (Mouse_CursorMoved, TIMER_AddTickHandler, MAPPER_AddHandler, PIC_FullIndex,
# Section_prop, control) resolves with the right types.
#
# What it does NOT prove: that DOSBox-X links or runs. It borrows the MSVC
# configuration in vs/config.h and stubs the two SDL headers that DOSBox-X's
# own headers pull in. Treat a pass as "no typos", not "it works".

set -eu

DOSBOX=${1:-}
if [ -z "$DOSBOX" ]; then
    echo "usage: $0 /path/to/dosbox-x" >&2
    exit 2
fi
if [ ! -f "$DOSBOX/src/ints/vfx1_mouse.cpp" ] ||
   [ ! -f "$DOSBOX/src/ints/vfx1_source_webxr.cpp" ]; then
    echo "$DOSBOX is missing the VFX1 sources -- run apply.sh first" >&2
    exit 2
fi

CXX=${CXX:-g++}
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT

# DOSBox-X's normal config.h comes from configure. Borrow the checked-in MSVC
# one instead, with attributes turned on so its packed structs really are
# packed and dos_inc.h's layout assertions hold on GCC.
sed 's/^#undef C_HAS_ATTRIBUTE$/#define C_HAS_ATTRIBUTE 1/' \
    "$DOSBOX/vs/config.h" > "$D/config.h"
cp "$DOSBOX/vs/config_package.h" "$D/config_package.h"

cat > "$D/SDL.h" <<'STUB'
/* Stand-in for SDL.h. Only enough for a syntax check; never a real build. */
#ifndef VFX_FAKE_SDL_H
#define VFX_FAKE_SDL_H
#include <stdint.h>
typedef uint32_t Uint32; typedef int32_t Sint32;
typedef uint16_t Uint16; typedef int16_t Sint16;
typedef uint8_t  Uint8;  typedef int8_t  Sint8;
typedef uint64_t Uint64; typedef int64_t Sint64;
extern "C" Uint32 SDL_GetTicks(void);
#endif
STUB

cat > "$D/SDL_video.h" <<'STUB'
#ifndef VFX_FAKE_SDL_VIDEO_H
#define VFX_FAKE_SDL_VIDEO_H
#include <SDL.h>
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
#endif
STUB

cd "$DOSBOX"
for f in src/ints/vfx1_mouse.cpp src/ints/vfx1_source_webxr.cpp; do
    echo "syntax-check: $CXX -fsyntax-only $f"
    "$CXX" -fsyntax-only -std=gnu++17 -Wall -Wextra -Wno-unknown-pragmas \
        -include cstdint \
        -I"$D" -Iinclude -Isrc -Isrc/vfx -I. \
        "$f"
done

echo "syntax-check: OK -- modules parse and every DOSBox-X symbol resolves"
echo "syntax-check: this is not a build; see the header comment"
