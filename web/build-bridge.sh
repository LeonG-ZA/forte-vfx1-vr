#!/usr/bin/env bash
#
# build-bridge.sh -- compile libvfxbridge's mouse mapping to WebAssembly.
#
#   bash web/build-bridge.sh   (bash: emsdk_env.sh needs it)
#
# Produces build/web/vfxbridge.js and build/web/vfxbridge.wasm for
# web/vfx1.html. Needs emsdk (web/build-wasm.sh installs it at ~/emsdk).
#
# Also compiles the same sources natively and runs a smoke test through the
# exported API, so a broken shim is caught here rather than on a headset.

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
OUT="$ROOT/build/web"
SMOKE="$ROOT/build/bridge-smoke"   # not under build/web: nothing to serve
mkdir -p "$OUT" "$SMOKE"

SRCS="$ROOT/src/adapters/webxr/vfxweb.c $ROOT/src/bridge/mousemap.c $ROOT/src/bridge/vfxmath.c"

# --- native smoke test of the exported API ---------------------------------
cat > "$SMOKE/vfxweb_smoke.c" <<'SMOKE'
#include <math.h>
#include <stdio.h>
int vfxweb_update(double, double, double, double);
int vfxweb_dx(void);
void vfxweb_configure(double, double, double, int, int, int, int, int);
int main(void) {
    long total = 0; int i;
    vfxweb_configure(600, 600, 70, 0, 0, 1, 1, 200);
    for (i = 0; i <= 100; ++i) {          /* 1 radian of yaw in 100 steps */
        double a = (double)i / 100.0;
        vfxweb_update(cos(a / 2), 0, sin(a / 2), 0);
        total += vfxweb_dx();
    }
    printf("smoke: 1 rad yaw -> %ld counts (expect about -600)\n", total);
    return (total < -605 || total > -595) ? 1 : 0;
}
SMOKE
cc -std=c99 -O2 -Wall -Wextra -I"$ROOT/include" -I"$ROOT/src" \
   $SRCS "$SMOKE/vfxweb_smoke.c" -lm -o "$SMOKE/vfxweb_smoke"
"$SMOKE/vfxweb_smoke"

# --- WebAssembly ------------------------------------------------------------
# shellcheck disable=SC1091
. "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
emcc -std=c99 -O2 -Wall -Wextra -I"$ROOT/include" -I"$ROOT/src" $SRCS \
     -sMODULARIZE=1 -sEXPORT_NAME=VfxBridgeModule -sENVIRONMENT=web \
     -sEXPORTED_FUNCTIONS=_vfxweb_configure,_vfxweb_recenter,_vfxweb_update,_vfxweb_dx,_vfxweb_dy,_vfxweb_tracked_yaw_deg,_vfxweb_tracked_pitch_deg,_vfxweb_clamped_count \
     -o "$OUT/vfxbridge.js"

ls -la "$OUT/vfxbridge.js" "$OUT/vfxbridge.wasm"
