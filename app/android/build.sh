#!/bin/bash
#
# build.sh -- native libraries for the Quest app, in WSL or Linux.
#
#   bash app/android/build.sh
#
# Needs (see app/android/README.md): the Android NDK r27c at ~/android,
# the OpenXR loader and headers at ~/android, and DOSBox Pure at ~/dosbox-pure.
# Puts libvfx1quest.so, libdosbox_pure_libretro.so and libopenxr_loader.so in
# build/android/lib/arm64-v8a, ready for app/android/package.sh.

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
ANDROID_DIR=${ANDROID_DIR:-$HOME/android}
NDK=${NDK:-$ANDROID_DIR/android-ndk-r27c}
CORE=${CORE:-$HOME/dosbox-pure}
API=32
OUT="$ROOT/build/android/lib/arm64-v8a"
# Where the launcher lists and downloads games, a directory served by
# web/serve.py, e.g.  VFX1_SERVER_DIR=http://<pc-address>:8080/native/
VFX1_SERVER_DIR=${VFX1_SERVER_DIR:-}
# Where the app posts its log lines and crash reports, for a headset without
# adb: web/serve.py's /applog, e.g. http://<pc-address>:8080/applog
VFX1_LOG_URL=${VFX1_LOG_URL:-}
OBJ="$ROOT/build/android/obj"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang"

mkdir -p "$OUT" "$OBJ"

# ---- DOSBox Pure with the VFX1, arm64 with its dynamic recompiler ----------
sh "$ROOT/integration/dosbox-pure/apply.sh" "$CORE"
"$NDK/ndk-build" -C "$CORE/jni" APP_ABI=arm64-v8a APP_PLATFORM=android-$API -j"$(nproc)" > "$OBJ/core-build.log" 2>&1 \
    || { tail -30 "$OBJ/core-build.log"; exit 1; }
cp "$CORE/libs/arm64-v8a/libretro.so" "$OUT/libdosbox_pure_libretro.so"

# ---- OpenXR loader -----------------------------------------------------------
LOADER=$(find "$ANDROID_DIR/openxr-loader/aar" -path "*arm64-v8a*" -name libopenxr_loader.so | head -1)
cp "$LOADER" "$OUT/libopenxr_loader.so"

# ---- the app -------------------------------------------------------------------
GLUE="$NDK/sources/android/native_app_glue"
"$CC" -shared -fPIC -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -I"$ROOT/include" -I"$ROOT/third_party/libretro" -I"$ROOT/app/xr" -I"$GLUE" -I"$ANDROID_DIR/OpenXR-SDK/include" \
    "-DVFX1_SERVER_DIR=\"$VFX1_SERVER_DIR\"" \
    "-DVFX1_LOG_URL=\"$VFX1_LOG_URL\"" \
    "$HERE/src/main_android.c" "$ROOT/app/xr/xrapp.c" "$ROOT"/src/vfxapp/*.c \
    "$ROOT/src/vfxmenu/vfxmenu.c" "$ROOT/src/vfxmenu/vgafont16.c" "$GLUE/android_native_app_glue.c" \
    "$ROOT/src/vfxhost/vfxhost.c" "$ROOT/src/vfxdepth/vfxdepth.c" "$ROOT/src/retrohost/retrohost.c" \
    -L"$OUT" -lopenxr_loader -lEGL -lGLESv3 -laaudio -llog -landroid -lm -ldl \
    -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384 \
    -o "$OUT/libvfx1quest.so"

ls -la "$OUT"
