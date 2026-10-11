#!/bin/sh
#
# apply.sh -- graft the VFX1 head-tracking overlay onto a DOSBox-X checkout.
#
#   ./integration/dosbox-x/apply.sh /path/to/dosbox-x
#
# Idempotent: running it twice changes nothing the second time, so it is safe
# after a `git pull` in the DOSBox-X tree.
#
# It is a script rather than a patch file on purpose. A patch pins line numbers
# in a tree that sees dozens of commits a week; anchored edits survive that,
# and when an anchor really is gone the script says which one and stops instead
# of half-applying.
#
# What it does:
#   1. copies the two new DOSBox-X source files into place
#   2. copies libvfxbridge in as src/vfx/ with its own Makefile.am
#   3. wires it into configure.ac, src/Makefile.am and src/ints/Makefile.am
#   4. adds the [vfx1] config section to src/dosbox.cpp
#   5. calls VFX1_Init() from the end of MOUSE_Init()

set -eu

DOSBOX=${1:-}
if [ -z "$DOSBOX" ]; then
    echo "usage: $0 /path/to/dosbox-x" >&2
    exit 2
fi
if [ ! -f "$DOSBOX/configure.ac" ] || [ ! -f "$DOSBOX/src/ints/mouse.cpp" ]; then
    echo "$DOSBOX does not look like a DOSBox-X checkout" >&2
    exit 2
fi

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)

echo "vfx1: source tree $ROOT"
echo "vfx1: target tree $DOSBOX"

# --- 1. the two new DOSBox-X files ------------------------------------------
mkdir -p "$DOSBOX/src/ints"
cp "$HERE/overlay/include/vfx1.h"                 "$DOSBOX/include/vfx1.h"
cp "$HERE/overlay/src/ints/vfx1_mouse.cpp"        "$DOSBOX/src/ints/vfx1_mouse.cpp"
cp "$HERE/overlay/src/ints/vfx1_source_webxr.cpp" "$DOSBOX/src/ints/vfx1_source_webxr.cpp"
mkdir -p "$DOSBOX/share/vfx1"
cp "$HERE/overlay/share/vfx1-webxr.html"          "$DOSBOX/share/vfx1/vfx1-webxr.html"

# --- 2. libvfxbridge as a subdirectory library ------------------------------
mkdir -p "$DOSBOX/src/vfx/vfx"
cp "$ROOT/src/bridge/vfxbridge.c" "$ROOT/src/bridge/stereo.c" \
   "$ROOT/src/bridge/reproject.c" "$ROOT/src/bridge/vfxmath.c" \
   "$ROOT/src/bridge/mousemap.c" \
   "$ROOT/src/bridge/vfxmath.h" "$ROOT/src/bridge/stereo_internal.h" \
   "$DOSBOX/src/vfx/"
cp "$ROOT/include/vfx/vfxcore_api.h" "$ROOT/include/vfx/vfxbridge.h" \
   "$ROOT/include/vfx/vfxmouse.h" \
   "$DOSBOX/src/vfx/vfx/"
# wsproto is a separate archive on purpose: it is a transport, and the bridge
# is required not to be one. Keeping them apart here keeps that visible.
cp "$ROOT/src/wsproto/wsproto.c" "$ROOT/src/wsproto/wsproto.h" "$DOSBOX/src/vfx/"

cat > "$DOSBOX/src/vfx/Makefile.am" <<'MAKEFILE'
# libvfxbridge, vendored by integration/dosbox-x/apply.sh. Do not edit here;
# edit the originals in the DOSBox VFX1 bridge repository and re-run apply.sh.
AM_CPPFLAGS = -I$(srcdir)

noinst_LIBRARIES = libvfxbridge.a libwsproto.a
libvfxbridge_a_SOURCES = vfxbridge.c stereo.c reproject.c vfxmath.c mousemap.c \
                         vfxmath.h stereo_internal.h \
                         vfx/vfxcore_api.h vfx/vfxbridge.h vfx/vfxmouse.h
libwsproto_a_SOURCES = wsproto.c wsproto.h
MAKEFILE

# --- 3-5. anchored edits -----------------------------------------------------
python3 - "$DOSBOX" <<'PYTHON'
import io, os, sys

dosbox = sys.argv[1]
changed = []

def edit(relpath, anchor, insert, where="after", marker=None):
    """Insert `insert` next to `anchor` in relpath, once."""
    path = os.path.join(dosbox, relpath)
    with io.open(path, encoding="utf-8", errors="surrogateescape") as f:
        s = f.read()
    tag = marker or insert.strip().splitlines()[0]
    if tag in s:
        return False
    if anchor not in s:
        sys.stderr.write("apply.sh: anchor not found in %s:\n  %s\n"
                         % (relpath, anchor.strip().splitlines()[0]))
        sys.exit(1)
    if where == "after":
        s = s.replace(anchor, anchor + insert, 1)
    elif where == "replace":
        s = s.replace(anchor, insert, 1)
    elif where == "replace_all":
        s = s.replace(anchor, insert)
    else:
        s = s.replace(anchor, insert + anchor, 1)
    with io.open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(s)
    changed.append(relpath)
    return True

# configure.ac: generate src/vfx/Makefile
edit("configure.ac", "src/ints/Makefile\n", "src/vfx/Makefile\n",
     marker="src/vfx/Makefile")

# src/Makefile.am: build and link the new library
edit("src/Makefile.am",
     "SUBDIRS = cpu debug dos fpu gui hardware libs ints misc shell builtin platform aviwriter output agent",
     " vfx", marker="platform aviwriter output agent vfx")
edit("src/Makefile.am",
     "bin_PROGRAMS = dosbox-x\n",
     "\n# VFX1 head tracking (integration/dosbox-x/apply.sh)\ndosbox_x_LDADD_vfx = vfx/libvfxbridge.a vfx/libwsproto.a\n",
     marker="dosbox_x_LDADD_vfx")
edit("src/Makefile.am",
     "\t\t   misc/libmisc.a output/liboutput.a hardware/mame/libmame.a libs/zmbv/libzmbv.a libs/decoders/internal/libopusint.a dos/libdos.a ints/libints.a agent/libagent.a",
     " $(dosbox_x_LDADD_vfx)", marker="agent/libagent.a $(dosbox_x_LDADD_vfx)")

# src/ints/Makefile.am: compile the module, and let it see the bridge headers
edit("src/ints/Makefile.am",
     "AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src",
     " -I$(top_srcdir)/src/vfx", marker="-I$(top_srcdir)/src/vfx")
edit("src/ints/Makefile.am",
     "libints_a_SOURCES = mouse.cpp",
     " vfx1_mouse.cpp vfx1_source_webxr.cpp",
     marker="vfx1_source_webxr.cpp")

# src/ints/mouse.cpp: let the module see its own header
edit("src/ints/mouse.cpp", '#include "mouse.h"\n', '#include "vfx1.h"\n',
     marker='#include "vfx1.h"')

# src/ints/mouse.cpp, gate 1 of 2: accumulate mickeys for head tracking too.
# DOSBox-X only accumulates while the host cursor is captured, which head
# tracking is not and never will be.
edit("src/ints/mouse.cpp",
     """    if (user_cursor_locked) {
        /* either device reports relative motion ONLY, and therefore requires that the user
         * has captured the mouse */""",
     """    if (user_cursor_locked || VFX1_Mouse_Active()) {
        /* either device reports relative motion ONLY, and therefore requires that the user
         * has captured the mouse -- or that VFX1 head tracking is feeding it */""",
     where="replace",
     marker="user_cursor_locked || VFX1_Mouse_Active()")

# src/ints/mouse.cpp, gate 2 of 2: hand those mickeys back through INT 33h 0Bh.
# Without this the counters fill up and function 0Bh still returns zero, which
# is exactly what tests/dos/int33probe.asm caught.
edit("src/ints/mouse.cpp",
     "    const auto locked = MOUSE_IsLocked();",
     "    const auto locked = MOUSE_IsLocked() || VFX1_Mouse_Active();",
     where="replace",
     marker="MOUSE_IsLocked() || VFX1_Mouse_Active()")

# src/ints/mouse.cpp: start head tracking once INT 33h exists
edit("src/ints/mouse.cpp",
     '    AddVMEventFunction(VM_EVENT_RESET,AddVMEventFunctionFuncPair(MOUSE_OnReset));\n',
     '\n    /* VFX1 head tracking, INT 33h path. Must come after INT 33h is up. */\n'
     '    void VFX1_Init(void);\n'
     '    VFX1_Init();\n',
     marker="VFX1_Init();")

# src/gui/sdlmain.cpp, WebAssembly only: stop yielding to the browser on every
# emulated millisecond.
#
# DOSBox-X's Emscripten build calls emscripten_sleep(0) from GFX_Events, which
# runs once per emulated millisecond, and again per rendered frame. Each call
# unwinds the stack through Asyncify and waits on a browser timer that is
# clamped to about 4 ms, so every emulated millisecond cost about 7 ms of real
# time. Measured in headless Edge, emulation ran at 10-14% of real time whether
# the emulated CPU did 200 or 20000 cycles per millisecond, which rules out the
# interpreter and points at the yield. Yielding only once enough real time has
# passed keeps the page responsive and removes that ceiling.
YIELD_HELPER = """#if C_EMSCRIPTEN
/* VFX1 bridge: yield to the browser at most once per VFX1_YIELD_MS of real
 * time. See integration/dosbox-x/apply.sh for the measurement behind this. */
#ifndef VFX1_YIELD_MS
#define VFX1_YIELD_MS 12.0
#endif
static void vfx1_browser_yield(void) {
    static double last_yield_ms = 0.0;
    const double now = emscripten_get_now();
    if (now - last_yield_ms >= VFX1_YIELD_MS) {
        last_yield_ms = now;
        emscripten_sleep(0);
    }
}
#endif

"""
edit("src/gui/sdlmain.cpp",
     "void GFX_EndUpdate(const uint16_t *changedLines) {\n",
     YIELD_HELPER, where="before", marker="static void vfx1_browser_yield(void)")
edit("src/gui/sdlmain.cpp",
     "#if C_EMSCRIPTEN\n    emscripten_sleep(0);\n#endif\n",
     "#if C_EMSCRIPTEN\n    vfx1_browser_yield();\n#endif\n",
     where="replace_all", marker="    vfx1_browser_yield();\n")

# src/dosbox.cpp: the [vfx1] config section
VFX1_SECTION = r'''
    /* VFX1 head tracking (DOSBox VFX1 bridge, integration/dosbox-x). */
    secprop=control->AddSection_prop("vfx1",&Null_Init,true);

    Pbool = secprop->Add_bool("headtracking",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("Emulate the Forte VFX1 VRMOUSE TSR: feed head orientation to DOS games as INT 33h mouse motion.\n"
                    "This is the mouse-emulation path, which needs no VFX1 hardware emulation. Off by default.");
    Pbool->SetBasic(true);

    Pstring = secprop->Add_string("posesource",Property::Changeable::OnlyAtStart,"none");
    Pstring->Set_values(vfx1_posesources);
    Pstring->Set_help("Where head orientation comes from.\n"
                      "none     No tracking.\n"
                      "script   A deterministic yaw and pitch sweep, for testing the path without a headset.\n"
                      "webxr    A real headset, via a browser page this emulator serves on localhost.");
    Pstring->SetBasic(true);

    Pbool = secprop->Add_bool("track yaw",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Map head yaw to mouse X. The original VRMOUSE had a -XX flag to turn this off.");

    Pbool = secprop->Add_bool("track pitch",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Map head pitch to mouse Y. The original VRMOUSE had a -XY flag to turn this off.");

    Pbool = secprop->Add_bool("invert yaw",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Reverse the horizontal direction, for titles whose mouse look runs the other way.");

    Pbool = secprop->Add_bool("invert pitch",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Reverse the vertical direction. Frequently needed; which way round a title wants is per-title.");

    Pdouble = secprop->Add_double("yaw sensitivity",Property::Changeable::WhenIdle,600.0);
    Pdouble->Set_help("Mouse counts emitted per radian of head yaw. There is no universally correct value:\n"
                      "it depends on the game's own mouse sensitivity, so expect to tune this per title.");

    Pdouble = secprop->Add_double("pitch sensitivity",Property::Changeable::WhenIdle,600.0);
    Pdouble->Set_help("Mouse counts emitted per radian of head pitch.");

    Pdouble = secprop->Add_double("pitch limit",Property::Changeable::WhenIdle,70.0);
    Pdouble->Set_help("Degrees of pitch above and below level that are tracked, matching the real headset's +/-70.\n"
                      "Looking past the limit emits nothing, so coming back does not have to unwind phantom travel.");

    Pint = secprop->Add_int("max counts per update",Property::Changeable::WhenIdle,200);
    Pint->Set_help("Upper bound on mouse counts injected in one poll, so a tracking glitch cannot fling the view.\n"
                   "The excess is discarded rather than delayed. 0 disables the limit.");

    Pint = secprop->Add_int("poll rate",Property::Changeable::OnlyAtStart,250);
    Pint->Set_help("How many times a second to sample the pose source, from 1 to 1000.\n"
                   "Deliberately unrelated to the game's frame rate, as the real hardware was.");

    Pint = secprop->Add_int("webxr port",Property::Changeable::OnlyAtStart,8085);
    Pint->Set_help("Port for the WebXR pose server, used when posesource=webxr.\n"
                   "Bound to 127.0.0.1 only. Open http://localhost:<port>/ in the headset's browser.");
    Pint->SetBasic(true);

    Pstring = secprop->Add_string("webxr page",Property::Changeable::OnlyAtStart,"");
    Pstring->Set_help("Path to the WebXR page to serve. Empty means serve nothing, in which case\n"
                      "you host vfx1-webxr.html yourself. It is installed to share/vfx1/vfx1-webxr.html.");
    Pstring->SetBasic(true);
'''

edit("src/dosbox.cpp", '    secprop=control->AddSection_prop("ttf",&Null_Init,true);',
     VFX1_SECTION, where="before", marker='AddSection_prop("vfx1"')

# the value list the section refers to
edit("src/dosbox.cpp",
     "    secprop=control->AddSection_prop(\"keyboard\",&Null_Init);",
     '    const char* vfx1_posesources[] = { "none", "script", "webxr", 0 };\n',
     where="before", marker="vfx1_posesources[]")

if changed:
    print("vfx1: edited " + ", ".join(sorted(set(changed))))
else:
    print("vfx1: already applied, nothing to do")
PYTHON

echo "vfx1: done. Next:"
echo "  cd $DOSBOX && ./autogen.sh && ./configure && make -j\$(nproc)"
echo "  then set headtracking=true in the [vfx1] section of dosbox-x.conf, and"
echo "  either posesource=script (no headset) or posesource=webxr with"
echo "  webxr page=$DOSBOX/share/vfx1/vfx1-webxr.html"
