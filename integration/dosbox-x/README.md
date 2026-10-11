# DOSBox-X integration — INT 33h mouse path

Feed head orientation to DOS games as ordinary INT
33h mouse motion, reproducing what Forte's VRMOUSE TSR did. It needs no
knowledge of the VIP card register map, which is the one thing nobody has
reverse engineered, so it is the first part of this project that can actually
drive a real game.

DOSBox-X is not vendored here. This directory grafts onto a checkout of it.

## Apply

```bash
git clone --depth 1 https://github.com/joncampbell123/dosbox-x.git
./integration/dosbox-x/apply.sh /path/to/dosbox-x
```

`apply.sh` is idempotent, so it is safe to re-run after pulling upstream. It
copies the new files in, vendors libvfxbridge and libwsproto into `src/vfx/`,
and makes small anchored edits to five upstream files. If upstream moves an anchor the script names the missing
one and stops rather than half-applying.

| File | Change |
| --- | --- |
| `include/vfx1.h` | new |
| `src/ints/vfx1_mouse.cpp` | new, transport-neutral |
| `src/ints/vfx1_source_webxr.cpp` | new, the WebXR pose source |
| `share/vfx1/vfx1-webxr.html` | new, the page the headset's browser opens |
| `src/vfx/` | libvfxbridge and libwsproto, vendored as two archives |
| `configure.ac` | one line, generate `src/vfx/Makefile` |
| `src/Makefile.am` | build and link the new library |
| `src/ints/Makefile.am` | compile the module, add an include path |
| `src/dosbox.cpp` | the `[vfx1]` config section |
| `src/ints/mouse.cpp` | one include, two widened lock gates, and three lines calling `VFX1_Init()` |

## Build

DOSBox-X needs its own dependencies, which this project does not install for
you. On Ubuntu 24.04, this is the list from DOSBox-X's own BUILD.md, and every
package in it was confirmed to exist:

```bash
sudo apt install build-essential libtool autoconf automake autogen pkg-config nasm libncurses-dev libsdl2-dev libsdl2-net-dev libsdl-net1.2-dev libpcap-dev libslirp-dev fluidsynth libfluidsynth-dev libavformat-dev libavcodec-dev libavcodec-extra libavutil-dev libswscale-dev libfreetype-dev libxkbfile-dev libxrandr-dev libpng-dev zlib1g-dev
```

Then:

```bash
cd /path/to/dosbox-x
./autogen.sh && ./configure --enable-sdl2 && make -j$(nproc)
```

## Configure

The section is off by default. In `dosbox-x.conf`:

```ini
[vfx1]
headtracking = true
posesource   = script
```

`posesource = script` runs a deterministic yaw and pitch sweep, so you can
launch a game and watch the view pan with no headset attached. That is the
point of it: it proves the INT 33h path end to end before any tracker exists.
`posesource = none` is tracking off, and `posesource = webxr` is a real
headset, described next.

Everything else is tuning: `yaw sensitivity` and `pitch sensitivity` in counts
per radian, `invert yaw` and `invert pitch`, `track yaw` and `track pitch`
(the VRMOUSE `-XX` and `-XY` flags), `pitch limit` in degrees, `max counts per
update`, and `poll rate`. Expect to change the sensitivities per title; see Q8
in [FINDINGS](../../docs/FINDINGS.md).

Bind a key to `vfx1recenter` in the mapper to make wherever you are looking the
new forward.

## Using a real headset

Set the pose source to `webxr` and point it at the installed page:

```ini
[vfx1]
headtracking = true
posesource   = webxr
webxr port   = 8085
webxr page   = /path/to/dosbox-x/share/vfx1/vfx1-webxr.html
```

Start DOSBox-X. Its log says `webxr pose server on http://localhost:8085/`.
Then, with the headset connected and its OpenXR runtime running, open
`http://localhost:8085/` in a WebXR-capable browser on the same machine and
press Start. The page shows the connection, the pose rate, and live yaw and
pitch, and has a Recentre button.

How the pieces fit:

- **The emulator serves the page itself.** WebXR only runs in a secure context,
  and `http://localhost` counts as one, so no certificate is needed.
- **The page streams the head orientation over a WebSocket** on the same port,
  one short text message per XR frame. `src/wsproto` holds the framing and the
  message format, with no I/O in it, and is unit tested.
- **The listener binds to 127.0.0.1 only** and takes one client at a time. It
  is not a web server and should never be exposed beyond the machine.
- **DOSBox-X in WSL works with a browser on Windows.** WSL forwards localhost,
  so the Windows browser reaches the emulator at `http://localhost:8085/`
  directly. Both the page load and the WebSocket upgrade were checked across
  that boundary.

The page is the only file in the project that names WebXR. Everything under
the socket is transport-neutral, which is the layering rule, and the
layering gate checks it.

## The end-to-end test

```bash
./tests/dos/run-dosbox-x.sh /path/to/dosbox-x/src/dosbox-x
```

This is the test that matters. It assembles
[int33probe.asm](../../tests/dos/int33probe.asm), a 16-bit COM program written
from scratch so that proving the path needs no copyrighted game binary, boots
DOSBox-X twice under the dummy video driver, and runs the probe inside DOS. The
probe polls INT 33h function 0Bh for six seconds and reports both net
displacement and total distance travelled per axis.

Two runs, because one proves less: with tracking on there must be motion, and
with tracking off there must be none. A stray host mouse event would satisfy
the first check on its own.

Current result on this tree:

| run | horizontal travel | vertical travel |
| --- | --- | --- |
| `headtracking = true` | 1378 mickeys | 661 mickeys |
| `headtracking = false` | 0 | 0 |

The WebXR path has its own version of the same test:

```bash
./tests/dos/run-webxr.sh /path/to/dosbox-x/src/dosbox-x
```

It boots DOSBox-X with `posesource = webxr`, connects
[webxr_client.py](../../tests/dos/webxr_client.py), which speaks exactly what
the page speaks, and runs the probe. The control run has nothing connect, so
motion can only have come over the socket. It also fetches the page over HTTP.

| run | horizontal travel | vertical travel |
| --- | --- | --- |
| client streaming poses | 1382 mickeys | 646 mickeys |
| nothing connected | 0 | 0 |

What this cannot test is WebXR itself. A pass means the socket, the framing,
the message format and the mouse injection all work, and only the browser half
remains unverified until a headset is on someone's head.

## Verifying without building DOSBox-X

The conversion arithmetic is in libvfxbridge and is fully tested there:

```bash
make check
```

To see what the path would inject over a scripted head motion:

```bash
./build/vfx-headless --mouse-trace --seconds 4 --hmd-hz 250
```

To type-check the DOSBox-X module against real DOSBox-X headers without the
emulator's dependencies:

```bash
./integration/dosbox-x/syntax-check.sh /path/to/dosbox-x
```

That borrows the checked-in MSVC configuration and stubs two SDL headers, so a
pass means "the module parses and every DOSBox-X symbol resolves with the right
types". It does not mean DOSBox-X links or runs.

## The two lock gates

DOSBox-X gates its INT 33h relative-motion path on the host cursor being
captured, and it does so in two separate places:

- `Mouse_CursorMoved` only accumulates mickeys when `user_cursor_locked`.
- `Mouse_Read_Motion_Data` only hands them back when `MOUSE_IsLocked()`.

Head tracking is not the host cursor and has to work whether or not the user
has clicked into the window, so `apply.sh` widens both to
`|| VFX1_Mouse_Active()`.

Both are needed, and finding that out is exactly what the DOS probe was for.
An earlier version of this integration widened only the first, by saving and
forcing the global around the injection. The counters filled up correctly and
INT 33h still returned zero, because the second gate is read at INT 33h time
rather than at injection time. The probe reported `X=0 Y=0` and the control run
reported the same, which is what sent us looking. Nothing short of running real
DOS code would have caught it.

## Known limitations

**The mouse path is open loop.** Nothing can read back the view angle a game
arrived at, so if a title scales or clamps mouse input differently than the
sensitivity assumes, head angle and view angle drift apart and only the
recentre key fixes it. This is inherent to mouse emulation, not a bug here, and
it is the strongest argument for the native VFX1 path.

**Counts are scaled twice.** The `yaw sensitivity` and `pitch sensitivity`
settings produce counts, and DOSBox-X then applies its own mouse scaling on top
before a game sees mickeys. The numbers a game reads are therefore not the
numbers this module emitted, which is why tuning is empirical.

**The WebXR source polls its socket on the emulator's millisecond tick,**
decimated to `poll rate`. Poses arrive at the headset's frame rate, so at the
default 250 Hz poll nothing is lost and the latest pose always wins. A future
source that has to block would need its own thread.

**The page has only been exercised by a stand-in client.** Nobody has yet run
it in a browser with a headset attached. The quaternion is passed through
unchanged on the grounds that WebXR and this project share a convention:
right-handed, +Y up, looking down -Z. If a real session shows yaw or pitch
reversed, that assumption is the first suspect, and the `invert` options work
around it meanwhile.

**Windows builds of this source have never been compiled.** It has a Winsock
path alongside the POSIX one, but only the POSIX side has been built and run.

**A standalone headset's own browser cannot reach it as-is.** The listener is
loopback-only, and a plain `http://` address on the LAN is not a secure
context. Forwarding the headset's localhost to the PC, for example with
`adb reverse tcp:8085 tcp:8085` on a Quest, should work in principle but is
untested.
