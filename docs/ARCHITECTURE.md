# Architecture

One C codebase serves the Quest app, the Windows apps and, in part, the WebXR
page. Code is split by what it depends on, so a fix is made once.

```
  DOS game (unmodified)
        |  INT 33h (Forte driver), I/O ports (VIP card), COM ports (trackers)
  ------+---------------------------------------------------------------
  emulated devices          src/vfx1dev, src/vrtrack           plain C
  DOSBox glue               integration/dosbox-common          C++, one file
  emulator                  DOSBox Pure (libretro) | js-dos    patched by apply.sh
        |  libretro API, plus the core's exported vfx1Pose()
  ------+---------------------------------------------------------------
  libretro frontend         src/retrohost
  session and launcher      src/vfxapp, src/vfxmenu            no XR, no graphics
  stereo, angles, controls  src/vfxhost
  fake 3D                   src/vfxdepth
        |  per frame: controllers, head pose, clock in; eye images, menu out
  ------+---------------------------------------------------------------
  OpenXR app                app/xr                             OpenXR, GL / GLES
  platform                  app/android | app/windows          the OS
```

## The emulated devices

`src/vfx1dev` is the VFX1: the Forte driver's INT 33h "Sense" interface and
the VIP card's registers (docs/FINDINGS.md Q1, Q13). `src/vrtrack` is the
i-glasses! and CyberMaxx serial head trackers (Q14). Both are plain C with
unit tests and know nothing about DOSBox.

`integration/dosbox-common/vfx1_dosbox.cpp` connects them to a DOSBox: it
hooks INT 33h ahead of the mouse driver, registers the card's ports, adds the
serial port types, and exports `vfx1Pose()`, through which the host passes
head and CyberPuck orientation. The same file is used by DOSBox Pure and by
js-dos; each has an `apply.sh` that copies it in and adds the hooks.

## The session

`src/vfxapp` is everything an app does between the emulator and the headset:

- the launcher (game list, downloads, settings saved per game),
- running the core through `src/retrohost`, a minimal libretro frontend,
- turning the core's frames into eye images through `src/vfxhost`:
  VFX1 page assembly, interlaced-line splitting, side-by-side, and deciding
  whether a frame is stereo,
- the controller layout and CyberPuck (`src/vfxhost`),
- fake 3D for flat pictures on a worker thread (`src/vfxdepth`).

Each frame it takes the controllers, head orientation and a clock, and gives
back the image to show, whether it is stereo, and the menu's pixels. It calls
no XR or graphics API, so every app drives it the same way.

## The apps

`app/xr/xrapp.c` is the OpenXR app: session, swapchains, input, and drawing
the head-locked screen and the menu with OpenGL ES 3 (Quest) or desktop
OpenGL 3.3 (Windows). Platform files supply the rest: NativeActivity, EGL and
AAudio on Android; a window, WGL, waveOut and raw keyboard and mouse input on
Windows. VFX1 Desk (`app/windows/main_desktop.c`) drives the same session in
a plain window, without OpenXR.

The WebXR page (`web/vfx1.html`) predates the shared session: it runs js-dos
with the shared device code compiled to WebAssembly, and does its stereo and
controls in JavaScript.

## Layering gate

`tools/check_layering.sh`, part of `make check`, fails the build if the
device code or `libvfxbridge` names an XR, graphics or windowing API.

## Conventions

Right-handed, +Y up, the camera looks down -Z. Quaternions are `w, x, y, z`.
Euler order is yaw about Y, then pitch about X, then roll about Z, as the
VFX1 sensor reported. VFX1 angles are 65536 to a full turn.
