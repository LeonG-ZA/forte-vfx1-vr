#!/bin/sh
# check_layering.sh -- mechanical enforcement of the layering rule.
#
# Section 1: "zero compile-time or runtime dependency on WebXR (or any other
# specific XR API)". Section 6 offers the acceptance test: swapping adapters
# must touch only the adapter translation unit. That is easy to agree with in
# review and easy to violate by accident, so it is checked here and wired into
# `make check`.
#
# Rule: the emulator core and libvfxbridge may not name any XR runtime,
# graphics API, windowing system or browser binding. Adapters, and the
# DOSBox-X pose sources that play the same role, may name exactly one -- that
# is what makes them adapters rather than core.
#
# src/wsproto is checked too: it is a transport, so it must not creep into the
# bridge, and nothing protected may reference it.

set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root" || exit 1

PROTECTED="include/vfx src/bridge src/core_stub src/wsproto src/vfx1dev src/vfxhost src/vfxmenu"

# The DOSBox-X overlay has the same split: these two files are core-side and
# must stay neutral, while vfx1_source_*.cpp and the served page may not.
PROTECTED_FILES="integration/dosbox-x/overlay/include/vfx1.h
integration/dosbox-x/overlay/src/ints/vfx1_mouse.cpp"

# Case-insensitive whole-ish tokens. Kept as an alternation so one grep pass
# reports every offence at once rather than stopping at the first.
FORBIDDEN='webxr|openxr|xrsession|xrframe|xrviewerpose|xrwebgllayer|xr_|XrInstance\
|emscripten|EM_JS|EM_ASM|__EMSCRIPTEN__\
|<GL/|<GLES|gl[A-Z][A-Za-z]*\(|EGL|WGL|vulkan|<vulkan|VkInstance|d3d11|d3d12|directx\
|SDL\.h|SDL2|SDL_[A-Z]\
|windows\.h|X11/|cocoa|objc/|<jni\.h>'

status=0

echo "layering: scanning $PROTECTED"

for dir in $PROTECTED; do
    [ -d "$dir" ] || continue
    hits=$(grep -rInE "$FORBIDDEN" "$dir" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "layering: FORBIDDEN reference below the bridge line:"
        echo "$hits" | sed 's/^/    /'
        status=1
    fi
done

for f in $PROTECTED_FILES; do
    [ -f "$f" ] || continue
    hits=$(grep -InE "$FORBIDDEN" "$f" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "layering: FORBIDDEN reference in a core-side overlay file:"
        echo "$hits" | sed "s|^|    $f:|"
        status=1
    fi
done

# The bridge must not reach for the transport library either.
if [ -d src/bridge ]; then
    hits=$(grep -rIn "wsproto" src/bridge include/vfx 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "layering: libvfxbridge references the transport library:"
        echo "$hits" | sed 's/^/    /'
        status=1
    fi
fi

# The core contract header must stay compilable with nothing but the C library.
BAD_INCLUDES=$(grep -hoE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<[^>]+>' \
    include/vfx/*.h src/bridge/*.h src/bridge/*.c src/core_stub/*.h src/core_stub/*.c \
    2>/dev/null | sed -E 's/.*<([^>]+)>.*/\1/' | sort -u \
    | grep -vE '^(stdint|stddef|stdlib|string|math|stdio|limits|float|stdbool|assert|inttypes|pthread)\.h$' \
    || true)

if [ -n "$BAD_INCLUDES" ]; then
    echo "layering: non-standard system headers below the bridge line:"
    echo "$BAD_INCLUDES" | sed 's/^/    /'
    status=1
fi

# Each adapter is allowed its own XR API, but only one, and only inside itself.
if [ -d src/adapters ]; then
    for a in src/adapters/*/; do
        [ -d "$a" ] || continue
        n=$(grep -rIloE 'webxr|openxr|emscripten' "$a" 2>/dev/null | wc -l | tr -d ' ')
        echo "layering: adapter $(basename "$a") -- $n file(s) naming an XR API"
    done
fi

# Same report for the DOSBox-X pose sources, which are adapters by another name.
for src in integration/dosbox-x/overlay/src/ints/vfx1_source_*.cpp; do
    [ -f "$src" ] || continue
    apis=$(grep -oiE 'webxr|openxr|emscripten' "$src" 2>/dev/null            | tr "A-Z" "a-z" | sort -u | tr "
" " ")
    echo "layering: pose source $(basename "$src") -- names: ${apis:-none}"
done

if [ "$status" -eq 0 ]; then
    echo "layering: OK -- core and bridge are transport-neutral"
fi
exit "$status"
