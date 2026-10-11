#!/bin/sh
#
# apply.sh -- add an emulated Forte VFX1 to DOSBox Pure, the libretro core the
# native OpenXR app loads (docs/ARCHITECTURE.md).
#
#   ./integration/dosbox-pure/apply.sh /path/to/dosbox-pure
#
# The same device and the same glue as the js-dos build (integration/jsdos):
# libvfx1dev and integration/dosbox-common/vfx1_dosbox.cpp. Idempotent and
# anchored, like the other apply scripts.
#
# What it does:
#   1. copies the glue to src/ints/vfx1_dosbox.cpp and libvfx1dev, as C++, to
#      src/ints/vfx1/; DOSBox Pure's Makefile and jni/Android.mk pick up
#      src/*/*.cpp and src/*/*/*.cpp by wildcard, so no build file changes
#   2. calls VFX1_Int33() first in INT 33h and VFX1_Init() from MOUSE_Init()
#   3. adds serial port types iglasses and cybermaxx, the rival headsets'
#      head trackers (libvrtrack, integration/dosbox-common/vrtrack_serial.cpp)
#
# The host passes head orientation by calling vfx1Pose() in the loaded core.

set -eu

CORE=${1:-}
if [ -z "$CORE" ] || [ ! -f "$CORE/dosbox_pure_libretro.cpp" ] || [ ! -f "$CORE/src/ints/mouse.cpp" ]; then
    echo "usage: $0 /path/to/dosbox-pure" >&2
    exit 2
fi

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)

mkdir -p "$CORE/src/ints/vfx1/vfx"
cp "$ROOT/integration/dosbox-common/vfx1_dosbox.cpp" "$CORE/src/ints/vfx1_dosbox.cpp"
cp "$ROOT/src/vfx1dev/vfx1dev.c"                     "$CORE/src/ints/vfx1/vfx1dev.cpp"
cp "$ROOT/include/vfx/vfx1dev.h"                     "$CORE/src/ints/vfx1/vfx/vfx1dev.h"
cp "$ROOT/src/vrtrack/vrtrack.c"                     "$CORE/src/ints/vfx1/vrtrack.cpp"
cp "$ROOT/include/vfx/vrtrack.h"                     "$CORE/src/ints/vfx1/vfx/vrtrack.h"
cp "$ROOT/integration/dosbox-common/vrtrack_serial.cpp" "$CORE/src/hardware/serialport/vrtrack_serial.cpp"

python3 - "$CORE" <<'PYTHON'
import sys

core = sys.argv[1]

def edit(rel, marker, anchor, replacement):
    path = f"{core}/{rel}"
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if marker in text:
        print(f"vfx1: {rel}: already applied")
        return
    if text.count(anchor) != 1:
        sys.exit(f"vfx1: {rel}: anchor not found exactly once:\n{anchor}")
    with open(path, "w", encoding="utf-8") as f:
        f.write(text.replace(anchor, replacement))
    print(f"vfx1: {rel}: edited")

edit("src/ints/mouse.cpp", "VFX1_Int33",
     "static Bitu INT33_Handler(void) {\n",
     "bool VFX1_Int33(void);\n"
     "void VFX1_Init(void);\n\n"
     "static Bitu INT33_Handler(void) {\n"
     "\tif (VFX1_Int33()) return CBRET_NONE;   /* VFX1 Sense calls share INT 33h */\n")

edit("src/ints/mouse.cpp", "VFX1_Init();",
     "\treal_writed(0,0x33<<2,i33loc);\n",
     "\treal_writed(0,0x33<<2,i33loc);\n"
     "\tVFX1_Init();\n")

edit("src/hardware/serialport/serialport.cpp", "VRTRACK_CreateSerial(i",
     "\t\t\tif (type==\"dummy\") {\n",
     "\t\t\tif ((serialports[i] = VRTRACK_CreateSerial(i, &cmd, type)) != NULL) {\n"
     "\t\t\t}\n"
     "\t\t\telse if (type==\"dummy\") {\n")

edit("src/hardware/serialport/serialport.cpp", "CSerial *VRTRACK_CreateSerial",
     "class SERIALPORTS:public Module_base {\n",
     "CSerial *VRTRACK_CreateSerial(Bitu id, CommandLine *cmd, const std::string &type);\n\n"
     "class SERIALPORTS:public Module_base {\n")

edit("src/dosbox.cpp", "\"iglasses\", \"cybermaxx\"",
     "\tconst char* serials[] = { \"dummy\", \"disabled\",\n",
     "\tconst char* serials[] = { \"dummy\", \"disabled\", \"iglasses\", \"cybermaxx\",\n")

# A frontend that keeps the core loaded and switches games (the app's game
# list) must get each game's own save file. DOSBox Pure takes the content's
# name for the save only while dbp_wasloaded is false, and never clears it,
# so every later game saved into the first game's .pure.zip.
edit("dosbox_pure_libretro.cpp", "dbp_wasloaded = false; /* vfx1",
     "void retro_unload_game(void)\n{\n\tDBP_Shutdown();\n",
     "void retro_unload_game(void)\n{\n\tDBP_Shutdown();\n"
     "\tdbp_wasloaded = false; /* vfx1: the next game is new content, with its own save */\n")
PYTHON

echo "vfx1: applied to $CORE"
