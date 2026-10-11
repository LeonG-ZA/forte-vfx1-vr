#!/bin/bash
#
# build.sh -- the Windows OpenXR app, cross-compiled in WSL or Linux.
#
#   bash app/windows/build.sh
#
# Needs (see app/windows/README.md) llvm-mingw and the Khronos OpenXR loader
# for Windows under ~/win, and DOSBox Pure at ~/dosbox-pure. Puts vfx1xr.exe,
# dosbox_pure_libretro.dll and openxr_loader.dll in build/windows, ready to
# copy to the PC's own disk and run.
#
# The app's code is app/xr and src/ shared with the Quest build; only
# main_windows.c and xrgl_win.c are Windows-only.

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
WIN=${WIN:-$HOME/win}
MINGW=${MINGW:-$(ls -d "$WIN"/llvm-mingw-*/ | grep -v '\.tar' | tail -1)}
XRSDK=${XRSDK:-$WIN/openxr}
CORE=${CORE:-$HOME/dosbox-pure}
CORE_WIN=${CORE_WIN:-$HOME/dosbox-pure-win}
OUT="$ROOT/build/windows"
VFX1_SERVER_DIR=${VFX1_SERVER_DIR:-}
export PATH="$MINGW/bin:$PATH"
CC=x86_64-w64-mingw32-clang

mkdir -p "$OUT"

# ---- DOSBox Pure with the VFX1, as a Windows DLL -----------------------------
# Its own copy of the tree, so the Quest's and Linux's objects stay apart.
if [ "${SKIP_CORE:-0}" != 1 ]; then
    sh "$ROOT/integration/dosbox-pure/apply.sh" "$CORE"
    rsync -a --exclude build --exclude '*.o' --exclude '*.so' --exclude libs --exclude obj "$CORE/" "$CORE_WIN/"
    # upstream memory.cpp uses std::nothrow without <new>, which libc++ needs;
    # libc++ linked in, so the DLL needs nothing beside it
    make -C "$CORE_WIN" ISWIN=1 CXX="x86_64-w64-mingw32-clang++ -include new" STRIP=x86_64-w64-mingw32-strip LDLIBS="-static -pthread" -j"$(nproc)" \
        > "$OUT/core-build.log" 2>&1 || { tail -30 "$OUT/core-build.log"; exit 1; }
    cp "$CORE_WIN/dosbox_pure_libretro.dll" "$OUT/"
fi

# ---- OpenXR loader -------------------------------------------------------------
cp "$XRSDK/x64/bin/openxr_loader.dll" "$OUT/"

# ---- the apps --------------------------------------------------------------------
# vfx1xr.exe, the OpenXR app; vfx1desk.exe, the desktop debug app. Both are
# the shared session plus winplat.c; only their main files differ.
CFLAGS=(-O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -I"$ROOT/include" -I"$ROOT/third_party/libretro" -I"$ROOT/app/xr" -I"$XRSDK/include"
    "-DVFX1_SERVER_DIR=\"$VFX1_SERVER_DIR\"")
SHARED=("$HERE/winplat.c" "$ROOT"/src/vfxapp/*.c
    "$ROOT/src/vfxmenu/vfxmenu.c" "$ROOT/src/vfxmenu/vgafont16.c"
    "$ROOT/src/vfxhost/vfxhost.c" "$ROOT/src/vfxdepth/vfxdepth.c" "$ROOT/src/retrohost/retrohost.c")
"$CC" "${CFLAGS[@]}" "$HERE/main_windows.c" "$HERE/xrgl_win.c" "$ROOT/app/xr/xrapp.c" "${SHARED[@]}" \
    "$OUT/openxr_loader.dll" -lopengl32 -lwinmm -lws2_32 -lgdi32 -static -pthread \
    -o "$OUT/vfx1xr.exe"
"$CC" "${CFLAGS[@]}" "$HERE/main_desktop.c" "$ROOT/src/adapters/headless/png_write.c" "${SHARED[@]}" \
    -lwinmm -lws2_32 -lgdi32 -static -pthread \
    -o "$OUT/vfx1desk.exe"

ls -la "$OUT"
