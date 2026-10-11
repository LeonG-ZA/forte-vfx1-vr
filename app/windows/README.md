# VFX1 XR for Windows: the same app on a PC headset

The Quest app built for Windows. It runs on any active OpenXR runtime:

- Virtual Desktop's VDXR, streaming to the Quest over Wi-Fi
- SteamVR
- Meta Quest Link

## One codebase

The Quest and Windows apps are the same program. Only the OS edges differ.

| Code | Where | Used by |
| --- | --- | --- |
| Session logic: launcher menu, controller layout, head, stereo detection, audio buffer | src/vfxapp, src/vfxhost, src/vfxmenu, src/retrohost | Quest, Windows, WebXR (as WebAssembly) |
| OpenXR session, input, swapchains, drawing, shaders | app/xr/xrapp.c | Quest, Windows |
| NativeActivity, EGL, AAudio, logcat | app/android/src/main_android.c | Quest |
| Console program, WGL, waveOut, log file | app/windows/main_windows.c, xrgl_win.c | Windows |

A fix to menus, stereo, controls or drawing goes in the shared files and
reaches both apps on the next build.

## Build

In WSL, with these under ~/win:

- llvm-mingw, from github.com/mstorsjo/llvm-mingw: an ucrt-ubuntu-x86_64 release, unpacked
- the Khronos OpenXR loader for Windows, `openxr_loader_windows-*.zip` from
  github.com/KhronosGroup/OpenXR-SDK-Source, unpacked as ~/win/openxr

DOSBox Pure must be at ~/dosbox-pure, as for the Quest build. Then run:

```bash
bash app/windows/build.sh
```

This writes `vfx1xr.exe`, `dosbox_pure_libretro.dll` and `openxr_loader.dll`
to build/windows. Set `SKIP_CORE=1` to rebuild only the app.

## Run

1. Start Virtual Desktop on the Quest and connect it to the PC.
2. Put game zips in `games\` next to `vfx1xr.exe`. These are the same zips the
   Quest uses. You can also give the program a folder:
   ```bash
   vfx1xr.exe C:\path\to\zips
   ```
3. Run `vfx1xr.exe`. If the headset is not streaming yet, the program waits
   for it. In the Virtual Desktop menu, VDXR should be the OpenXR runtime.

The controls and the menu are the same as on the Quest. The left controller's
menu button opens the launcher.

Logging:

- The program writes its log to `vfx1xr.log` next to itself.
- To also send log lines to the dev PC, set
  `VFX1_LOG_URL=http://host:8080/applog`.
- `VFX1_SERVER=http://host:8080/native/` sets where the menu's Download page
  lists games from.

## VFX1 Desk: the debug app without a headset

`vfx1desk.exe` sits beside `vfx1xr.exe` and runs the same shared session in a
plain window, for checking stereo, tracking and controls quickly.

- The game is drawn as a red/cyan anaglyph when it draws stereo. The view
  button switches to half-colour, grey, side by side, left eye or right eye.
  Swap eyes shows whether the eyes are the wrong way round.
- **Head pad.** Drag for yaw and pitch. The slider, or the mouse wheel over
  the pad, sets roll.
- **CyberPuck pad.** Drag to tilt. Dragging up tips the puck forward, like a
  joystick. Trigger, Grip and A work while held; right-click one to latch it.
- Right-click a pad to centre it. Recentre centres both and resets the
  tracker reference.
- The keyboard goes to DOS. Click the game to give DOS the mouse; F12 takes
  it back.
- F11, or the Menu button, opens the launcher. The arrows, Enter and
  Backspace drive it.
- The panel shows the image size, whether stereo was detected, the VFX1 poll
  counts and the driver's video mode.
- **Record 90 raw frames** saves the next 90 frames the core draws, every
  one, before any stereo processing, to `frames\` as PNGs with `frames.csv`.
  This is for working out how a title lays out its eyes.
- Keys pressed and released between two frames still reach DOS.
- It logs to `vfx1desk.log`.
