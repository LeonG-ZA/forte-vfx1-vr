#!/bin/sh
#
# apply.sh -- add an emulated Forte VFX1 to js-dos's classic DOSBox backend.
#
#   ./integration/jsdos/apply.sh /path/to/js-dos/emulators
#
# Expects a checkout of https://github.com/js-dos/emulators at the tag the
# page's js-dos release uses (v8.4.1 for js-dos 8.4.1), with the native/dosbox
# submodule. Idempotent, and anchored rather than a patch file, for the same
# reasons as integration/dosbox-x/apply.sh: a missing anchor stops the script
# with its name instead of half-applying.
#
# What it does:
#   1. copies jsdos-vfx1.cpp and libvfx1dev (as C++) into native/jsdos
#   2. adds both to the wdosbox sources
#   3. calls VFX1_Int33() first in INT 33h and VFX1_Init() from MOUSE_Init()
#   4. routes the page's "vfx1-pose" and "vfx1-stats" backend events
#   5. builds only the classic DOSBox targets, so the DOSBox-X and sokol
#      submodules are not needed
#   6. adds wall-clock profiling spans around the main loop's parts, read
#      with the "vfx1-prof" backend event (jsdos-vfx1-prof.h)
#   7. makes the optimisation flags selectable with -DVFX1_OPT, for example
#      "-O3 -msimd128 -fno-exceptions"

set -eu

EMU=${1:-}
if [ -z "$EMU" ] || [ ! -f "$EMU/targets/dosbox.cmake" ] || [ ! -f "$EMU/native/jsdos/jsdos-mouse.cpp" ]; then
    echo "usage: $0 /path/to/js-dos/emulators   (with native/dosbox checked out)" >&2
    exit 2
fi

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)

mkdir -p "$EMU/native/jsdos/vfx1/vfx"
cp "$ROOT/integration/dosbox-common/vfx1_dosbox.cpp" "$EMU/native/jsdos/jsdos-vfx1.cpp"
cp "$HERE/overlay/native/jsdos/jsdos-vfx1-prof.h"   "$EMU/native/jsdos/jsdos-vfx1-prof.h"
cp "$HERE/overlay/native/jsdos/jsdos-vfx1-prof.cpp" "$EMU/native/jsdos/jsdos-vfx1-prof.cpp"
cp "$ROOT/src/vfx1dev/vfx1dev.c"               "$EMU/native/jsdos/vfx1/vfx1dev.cpp"
cp "$ROOT/include/vfx/vfx1dev.h"               "$EMU/native/jsdos/vfx1/vfx/vfx1dev.h"

python3 - "$EMU" <<'PYTHON'
import sys

emu = sys.argv[1]

def edit(rel, marker, anchor, replacement):
    path = f"{emu}/{rel}"
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

edit("targets/dosbox.cmake", "jsdos-vfx1.cpp",
     '        "${NATIVE_DIR}/jsdos/jsdos-mouse.cpp"\n',
     '        "${NATIVE_DIR}/jsdos/jsdos-mouse.cpp"\n'
     '        "${NATIVE_DIR}/jsdos/jsdos-vfx1.cpp"\n'
     '        "${NATIVE_DIR}/jsdos/vfx1/vfx1dev.cpp"\n')

edit("native/jsdos/jsdos-mouse.cpp", "VFX1_Int33",
     "static Bitu INT33_Handler(void) {\n",
     "bool VFX1_Int33(void);\n"
     "void VFX1_Init(void);\n\n"
     "static Bitu INT33_Handler(void) {\n"
     "\tif (VFX1_Int33()) return CBRET_NONE;   /* VFX1 Sense calls share INT 33h */\n")

edit("native/jsdos/jsdos-mouse.cpp", "VFX1_Init();",
     "\treal_writed(0,0x33<<2,i33loc);\n",
     "\treal_writed(0,0x33<<2,i33loc);\n"
     "\tVFX1_Init();\n")

edit("src/dos/dosbox/cpp/worker-protocol.cpp", "vfx1-stats",
     '        console.log("backend event", json);\n'
     '        const message = JSON.parse(json);\n',
     '        const message = JSON.parse(json);\n'
     '        if (message.type === "vfx1-pose") {\n'
     '            Module._vfx1Pose(message.yaw | 0, message.pitch | 0, message.roll | 0,\n'
     '                             message.puckPitch | 0, message.puckRoll | 0, message.buttons | 0);\n'
     '            return;\n'
     '        }\n'
     '        if (message.type === "vfx1-stats") {\n'
     '            const p = Module._vfx1Stats() >> 2;\n'
     '            const s = Array.from(Module.HEAPU32.subarray(p, p + 9));\n'
     '            Module.sendMessage("ws-stdout", { message: "vfx1-stats " + JSON.stringify({\n'
     '                poses: s[0], psrWrites: s[1], psr: s[2], statusCalls: s[3],\n'
     '                dataCalls: s[4], reportCalls: s[5], zeroCalls: s[6], videoMode: s[7], vipWrites: s[8] }) });\n'
     '            return;\n'
     '        }\n'
     '        console.log("backend event", json);\n')

edit("CMakeLists.txt", "vfx1: classic DOSBox only",
     'include("${TARGETS_DIR}/dhry2.cmake")\n'
     'include("${TARGETS_DIR}/sokol.cmake")\n\n'
     '# tier 2\n'
     'include("${TARGETS_DIR}/dosbox-sdl.cmake")\n'
     'include("${TARGETS_DIR}/dosbox-x-sdl2.cmake")\n',
     '# vfx1: classic DOSBox only; the other targets need submodules not fetched\n'
     '# include("${TARGETS_DIR}/dhry2.cmake")\n'
     '# include("${TARGETS_DIR}/sokol.cmake")\n'
     '# include("${TARGETS_DIR}/dosbox-sdl.cmake")\n'
     '# include("${TARGETS_DIR}/dosbox-x-sdl2.cmake")\n')
# ---- 6. profiling spans ----------------------------------------------------
PROF = '/* vfx1-prof-include */\n#include "{}jsdos-vfx1-prof.h"\n'

edit("targets/dosbox.cmake", "jsdos-vfx1-prof.cpp",
     '        "${NATIVE_DIR}/jsdos/vfx1/vfx1dev.cpp"\n',
     '        "${NATIVE_DIR}/jsdos/vfx1/vfx1dev.cpp"\n'
     '        "${NATIVE_DIR}/jsdos/jsdos-vfx1-prof.cpp"\n')

edit("native/dosbox/src/dosbox.cpp", "vfx1-prof-include",
     '#include "dosbox.h"\n',
     '#include "dosbox.h"\n' + PROF.format("../../jsdos/"))
edit("native/dosbox/src/dosbox.cpp", "VFX1P_PIC",
     '        if (PIC_RunQueue()) {\n            ret = (*cpudecoder)();\n',
     '        VFX1_PROF_BEGIN(vfx1_t_pic);\n'
     '        bool vfx1_pic = PIC_RunQueue();\n'
     '        VFX1_PROF_END(VFX1P_PIC, vfx1_t_pic);\n'
     '        if (vfx1_pic) {\n'
     '            VFX1_PROF_BEGIN(vfx1_t_cpu);\n'
     '            ret = (*cpudecoder)();\n'
     '            VFX1_PROF_END(VFX1P_CPU, vfx1_t_cpu);\n')
edit("native/dosbox/src/dosbox.cpp", "VFX1P_CALLBACK",
     '\t\t\t\tBitu blah = (*CallBack_Handlers[ret])();\n',
     '\t\t\t\tVFX1_PROF_BEGIN(vfx1_t_cb);\n'
     '\t\t\t\tBitu blah = (*CallBack_Handlers[ret])();\n'
     '\t\t\t\tVFX1_PROF_END(VFX1P_CALLBACK, vfx1_t_cb);\n')
edit("native/dosbox/src/dosbox.cpp", "VFX1P_TIMER",
     '\t\t\t\tTIMER_AddTick();\n',
     '\t\t\t\tVFX1_PROF_BEGIN(vfx1_t_tick);\n'
     '\t\t\t\tTIMER_AddTick();\n'
     '\t\t\t\tVFX1_PROF_END(VFX1P_TIMER, vfx1_t_tick);\n')

edit("native/jsdos/jsdos-asyncify.cpp", "VFX1P_SLEEP",
     '  syncSleep(ms, nonSkippable);\n#else',
     '  { VFX1_PROF_BEGIN(vfx1_t_sleep);\n'
     '  syncSleep(ms, nonSkippable);\n'
     '  VFX1_PROF_END(VFX1P_SLEEP, vfx1_t_sleep); }\n#else')
edit("native/jsdos/jsdos-asyncify.cpp", "vfx1-prof-include",
     'extern "C" void asyncify_sleep(unsigned int ms, bool nonSkippable) {\n',
     PROF.format("") +
     'extern "C" void asyncify_sleep(unsigned int ms, bool nonSkippable) {\n')

edit("native/dosbox/src/hardware/vga_draw.cpp", "vfx1-prof-include",
     '#include "dosbox.h"\n',
     '#include "dosbox.h"\n' + PROF.format("../../../jsdos/"))
edit("native/dosbox/src/hardware/vga_draw.cpp", "VFX1P_VGADRAW",
     'static void VGA_DrawPart(Bitu lines) {\n',
     'static void VGA_DrawPart_Inner(Bitu lines);\n'
     'static void VGA_DrawPart(Bitu lines) {\n'
     '\tVFX1_PROF_BEGIN(vfx1_t_draw);\n'
     '\tVGA_DrawPart_Inner(lines);\n'
     '\tVFX1_PROF_END(VFX1P_VGADRAW, vfx1_t_draw);\n'
     '}\n'
     'static void VGA_DrawPart_Inner(Bitu lines) {\n')
edit("native/dosbox/src/hardware/vga_draw.cpp", "VGA_DrawSingleLine_Inner",
     'static void VGA_DrawSingleLine(Bitu /*blah*/) {\n',
     'static void VGA_DrawSingleLine_Inner(Bitu blah);\n'
     'static void VGA_DrawSingleLine(Bitu blah) {\n'
     '\tVFX1_PROF_BEGIN(vfx1_t_line);\n'
     '\tVGA_DrawSingleLine_Inner(blah);\n'
     '\tVFX1_PROF_END(VFX1P_VGADRAW, vfx1_t_line);\n'
     '}\n'
     'static void VGA_DrawSingleLine_Inner(Bitu /*blah*/) {\n')

edit("native/jsdos/dosbox/jsdos-main.cpp", "VFX1P_GFXEND",
     'void GFX_EndUpdate(const Bit16u *changedLines) {\n',
     PROF.format("../") +
     'static void GFX_EndUpdate_Inner(const Bit16u *changedLines);\n'
     'void GFX_EndUpdate(const Bit16u *changedLines) {\n'
     '    VFX1_PROF_BEGIN(vfx1_t_end);\n'
     '    GFX_EndUpdate_Inner(changedLines);\n'
     '    VFX1_PROF_END(VFX1P_GFXEND, vfx1_t_end);\n'
     '}\n'
     'static void GFX_EndUpdate_Inner(const Bit16u *changedLines) {\n')

edit("native/jsdos/dosbox/jsdos-mixer.cpp", "VFX1P_MIX",
     'static void MIXER_Mix(void) {\n',
     PROF.format("../") +
     'static void MIXER_Mix_Inner(void);\n'
     'static void MIXER_Mix(void) {\n'
     '\tVFX1_PROF_BEGIN(vfx1_t_mix);\n'
     '\tMIXER_Mix_Inner();\n'
     '\tVFX1_PROF_END(VFX1P_MIX, vfx1_t_mix);\n'
     '}\n'
     'static void MIXER_Mix_Inner(void) {\n')

edit("src/dos/dosbox/cpp/worker-protocol.cpp", "VFX1P_POST",
     '  emsc_end_frame_update(frameRgb, frameWidth, frameHeight);\n}\n',
     '  { VFX1_PROF_BEGIN(vfx1_t_post);\n'
     '  emsc_end_frame_update(frameRgb, frameWidth, frameHeight);\n'
     '  VFX1_PROF_END(VFX1P_POST, vfx1_t_post); }\n}\n')
edit("src/dos/dosbox/cpp/worker-protocol.cpp", "vfx1-prof-include",
     '#include <protocol.h>\n',
     '#include <protocol.h>\n' + PROF.format("../../../../native/jsdos/"))
edit("src/dos/dosbox/cpp/worker-protocol.cpp", 'message.type === "vfx1-prof"',
     '        if (message.type === "vfx1-stats") {\n',
     '        if (message.type === "vfx1-prof") {\n'
     '            const n = 9, p = Module._vfx1ProfTake();\n'
     '            const v = new Float64Array(Module.HEAPU8.buffer, p, 2 * n);\n'
     '            const names = ["pic", "cpu", "callback", "timer", "sleep", "vgaDraw", "gfxEnd", "post", "mix"];\n'
     '            const out = { ms: {}, calls: {} };\n'
     '            names.forEach((k, i) => { out.ms[k] = +v[i].toFixed(1); out.calls[k] = v[n + i]; });\n'
     '            Module.sendMessage("ws-stdout", { message: "vfx1-prof " + JSON.stringify(out) });\n'
     '            return;\n'
     '        }\n'
     '        if (message.type === "vfx1-stats") {\n')

# ---- 7. selectable optimisation level ---------------------------------------
edit("CMakeLists.txt", "VFX1_OPT",
     '\tadd_compile_options(-Oz)\n'
     '\tset(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Oz -flto=thin")\n',
     '\t# vfx1: optimisation level from -DVFX1_OPT; js-dos ships -Oz\n'
     '\tif (NOT VFX1_OPT)\n'
     '\t\tset(VFX1_OPT "-Oz")\n'
     '\tendif()\n'
     '\tseparate_arguments(VFX1_OPT_LIST UNIX_COMMAND "${VFX1_OPT}")\n'
     '\tadd_compile_options(${VFX1_OPT_LIST})\n'
     '\tset(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${VFX1_OPT} -flto=thin")\n')
PYTHON

echo "vfx1: applied to $EMU"
