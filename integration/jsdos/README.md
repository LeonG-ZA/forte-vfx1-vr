# js-dos integration: an emulated Forte VFX1

This adds a VFX1 to js-dos's classic DOSBox backend, the emulator that runs in
the Quest's browser. Native VFX1 titles then find what they expect:

- **Forte's driver on INT 33h.** Titles detect VFX1.COM and read the head
  tracker and CyberPuck through its "Sense" interface, which shares INT 33h
  with the mouse. The emulated driver answers detection, device data,
  configuration and report packets, zeroing, and video mode calls.
- **The VIP card's registers** at port 260h. Titles flip stereo pages through
  its page select register and download the palette. Writes are kept; nothing
  needs an interrupt.

The device logic is [src/vfx1dev](../../src/vfx1dev/vfx1dev.c), plain C with
unit tests in [tests/test_vfx1dev.c](../../tests/test_vfx1dev.c). This
directory only holds the DOSBox glue and the build.

## Build

In WSL or Linux, with emsdk 5.0.2 at `~/emsdk5`, cmake and ninja:

```bash
bash integration/jsdos/build.sh
```

That clones js-dos emulators v8.4.1 to `~/jsdos-emu`, applies
[apply.sh](apply.sh), builds `wdosbox`, and puts it in `build/web/emulators/`
next to the unchanged `emulators.js` from the js-dos CDN. The two must be the
same js-dos release, and the script checks.

js-dos 8.4.1 is built by its CI with Emscripten 5.0.2. Its Dockerfile names
3.1.28, but 3.1.28's JavaScript optimizer rejects the worker code.

## Use

[web/vfx1.html](../../web/vfx1.html) loads this build when native tracking is
on, with `pathPrefix: "emulators/"`:

- `stereo=vfx` runs Descent with `-vfx -vfxtrak`: native stereo and native
  head tracking.
- `game=sense` runs [tests/dos/senseprobe.asm](../../tests/dos/senseprobe.asm),
  which makes the SDK's calls and prints what came back.

The page sends head orientation with
`ci.sendBackendEvent({ type: "vfx1-pose", yaw, pitch, roll, puckPitch, puckRoll, buttons })`
in VFX1 units, and asks for driver statistics with
`ci.sendBackendEvent({ type: "vfx1-stats" })`, which come back on
`ci.events().onStdout` as a line starting `vfx1-stats `.

The build also carries a profiler: wall-clock spans around the parts of
DOSBox's main loop (events and VGA drawing, the CPU core, callbacks, timer
ticks and sound mixing, browser sleeps, frame conversion and posting).
`ci.sendBackendEvent({ type: "vfx1-prof" })` returns and clears them as a
`vfx1-prof ` line on `ci.events().onStdout`; web/vfx1.html adds them to its
telemetry every 2 seconds as milliseconds per real second.

Titles take the card's port from `SET VIPPORT=260`, which must match the port
in [vfx1_dosbox.cpp](../dosbox-common/vfx1_dosbox.cpp), the DOSBox glue shared with the native app.

## What apply.sh changes

1. Copies the shared DOSBox glue (integration/dosbox-common) and libvfx1dev, compiled as C++, into
   `native/jsdos`.
2. Adds both to the `wdosbox` sources.
3. Calls `VFX1_Int33()` first in the INT 33h handler and `VFX1_Init()` from
   `MOUSE_Init()`.
4. Routes the `vfx1-pose` and `vfx1-stats` backend events in the worker.
5. Leaves out the DOSBox-X, sokol and SDL targets, whose submodules this build
   does not fetch.
