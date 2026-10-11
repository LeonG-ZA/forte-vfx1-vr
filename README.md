# Forte VFX1 for modern VR headsets

Play the DOS games that were made for 1990s VR headsets, in stereo and with
head tracking, on a Meta Quest 3 or a PC VR headset.

The games run unmodified in [DOSBox Pure](https://github.com/schellingb/dosbox-pure).
This project adds the old headsets to the emulator, so a game finds the
hardware it expects, and shows the game's stereo picture in the modern
headset with head tracking from it.

## Emulated headsets

| Headset | What is emulated | Stereo the game draws |
| --- | --- | --- |
| **Forte VFX1 Headgear** (1995) | The VIP ISA card (ports at 260h, also 300h) and Forte's driver VFX1.COM on INT 33h: head tracker, CyberPuck, video modes and page selection | four 320x240 pages in a 640x480 screen, flipped per eye |
| **Virtual i-O i-glasses!** | The head tracker on a COM port | interlaced lines: even lines left eye, odd lines right |
| **Victormaxx CyberMaxx** | The head tracker on a COM port | interlaced lines |

The headset's orientation becomes the old head tracker's, and a controller
becomes the VFX1's CyberPuck (tilt and three buttons). The picture is shown
head-locked at the VFX1's own size: 35.5 degrees wide, 3.35 m away.

Games known to work, and the switches and settings each one needs, are in
[docs/GAMES.md](docs/GAMES.md). Descent, Descent 2, Quake, Z.A.R. (i-glasses!
mode), MadSpace, Gridfighter 3D and System Shock show stereo.

## The versions

All of them share one C codebase (`src/`); only the edges differ.

| Version | Runs on | What it is for |
| --- | --- | --- |
| **VFX1 Quest** (`app/android`) | Meta Quest 3, standalone | Playing. A native OpenXR app with DOSBox Pure's ARM64 recompiler. |
| **VFX1 XR** (`app/windows`, `vfx1xr.exe`) | Windows with any OpenXR runtime: Virtual Desktop (VDXR), SteamVR, Quest Link | Playing on a PC, streamed to the headset. Same app as the Quest's. |
| **VFX1 Desk** (`app/windows`, `vfx1desk.exe`) | Windows, no headset | Debugging. A plain window showing the stereo as an anaglyph (or side by side, either eye), mouse-driven head and CyberPuck pads, frame recording. |
| **WebXR** (`web/`), experimental | The Quest's browser | The first version: js-dos in the browser. Slower, since a browser cannot run a recompiler; kept for reference and not developed further. |

## Getting games in

A game is a **zip of the game's folder with a `DOSBOX.CONF` inside**. DOSBox
Pure runs the zip as drive C and the `[autoexec]` section starts the game
with its VR switches:

```
[joystick]
joysticktype=none

[autoexec]
SET VIPPORT=260 I5
CD DESCENT
dos32a DCNTSHR.EXE -vfx -vfxtrak
```

The VFX1 driver is built into the emulator, so VFX1.COM is not needed. CD
games can include their CD image (ISO, or BIN/CUE) in the zip; DOSBox Pure
mounts it. Use your own copies; no game files are included here.

Then put the zips where the app looks:

- **Quest:** in the app's files folder,
  `/sdcard/Android/data/com.fortevfx.vfx1quest/files/`, with adb:
  ```bash
  adb push mygame.zip /sdcard/Android/data/com.fortevfx.vfx1quest/files/
  ```
  or download them in the headset from a PC (below).
- **Windows:** in `games\` next to `vfx1xr.exe` and `vfx1desk.exe`, or pass a
  folder: `vfx1xr.exe C:\path\to\zips`.

Open the launcher (left controller's menu button; F11 in VFX1 Desk), pick the
game, and it starts. Each game keeps its own settings and its own DOSBox Pure
save file (`<game>.pure.zip`, which the launcher hides).

### Downloading games from a PC

`web/serve.py` serves a folder of zips to the headset over Wi-Fi. On the PC,
with Python 3:

```bash
python web/serve.py --dir path/to/zips --lan
```

It prints the address to use, such as `http://192.0.2.10:8080/`. Build the
app with that address (`VFX1_SERVER_DIR=http://192.0.2.10:8080/` for
`app/android/build.sh` or `app/windows/build.sh`), or on Windows set the
`VFX1_SERVER` environment variable. The launcher then has **Download games
from the PC...**, which lists the zips and downloads the one picked;
interrupted downloads resume. The server also takes the apps' log lines on
`/applog` (set `VFX1_LOG_URL=http://<pc>:8080/applog`), which is handy on a
headset without adb.

## Controls

On the Quest and in VFX1 XR (the default layout; the right controller is the
CyberPuck):

| Control | Does |
| --- | --- |
| Head | the VFX1 head tracker |
| Right controller tilt | CyberPuck tilt |
| Right trigger, grip, A | CyberPuck buttons; also Ctrl, A key and Enter |
| Right B | Esc |
| Right stick | arrow keys; click is Space |
| Left trigger | Space |
| Left stick | forward/back: A and Z keys; sideways: arrows |
| Left X | recentre the head |
| Left Y | mode change: auto, stereo, mono (configurable) |
| Left menu button | the launcher |

A Bluetooth keyboard and mouse work on the Quest, and the PC's keyboard and
mouse work in VFX1 XR (F12 releases the mouse).

## Settings

The launcher's **Settings** page applies to the running game, or to new
games when none is running:

- **Stereo**: off, VFX1 pages, interlaced lines.
- **Stereo or flat screen**: *fixed* (default) always shows what **Always
  show** says, stereo or flat; *detected (experimental)* decides per frame,
  so menus go flat. The mode-change key overrides either.
- **Fake 3D for flat screens (experimental)**: estimates depth from a flat
  picture with a small neural network (FastDepth) and shows it in stereo.
  Needs `fastdepth.vfd` in the games folder, made with
  `tools/depthtest/export_fastdepth.py`.
- **VFX1 LCD look**: the dotted look of the VFX1's LCD panels.
- **Size and distance** of the virtual screen.
- **CyberPuck hand**, **DOS mouse** from the controller (CyberPuck tilt or
  pointing), **head movement** (looks or steers), **head tracker on a COM
  port** for i-glasses! and CyberMaxx, and DOSBox Pure options per game
  (CPU, cycles and more).

## Building

The builds run in WSL (Ubuntu) on Windows, or on Linux. All need DOSBox Pure
with this project's changes applied:

```bash
git clone https://github.com/schellingb/dosbox-pure.git ~/dosbox-pure
sh integration/dosbox-pure/apply.sh ~/dosbox-pure
```

- **Tests:** `make check` (C99, make and libm only).
- **Quest:** `bash app/android/build.sh` in WSL, then
  `bash app/android/package.sh` in Git Bash on Windows; see
  [app/android/README.md](app/android/README.md).
- **Windows:** `bash app/windows/build.sh` in WSL, with llvm-mingw and the
  OpenXR loader; see [app/windows/README.md](app/windows/README.md).
- **WebXR:** see [integration/jsdos/README.md](integration/jsdos/README.md).

## Layout

| Path | What it is |
| --- | --- |
| `src/vfx1dev` | the VFX1 driver and VIP card, plain C |
| `src/vrtrack` | the i-glasses! and CyberMaxx serial head trackers |
| `integration/dosbox-common` | the glue that puts both into any DOSBox |
| `integration/dosbox-pure`, `integration/jsdos` | apply scripts for DOSBox Pure and js-dos |
| `src/vfxhost` | stereo page assembly, stereo detection, head and puck angles, controller layout |
| `src/vfxapp` | the app session and launcher, shared by every version |
| `src/retrohost` | a minimal libretro frontend |
| `src/vfxmenu` | the in-headset menu |
| `src/vfxdepth` | fake 3D: depth from a flat picture |
| `app/xr` | the OpenXR app, shared by Quest and Windows |
| `app/android`, `app/windows` | the platform edges |
| `web` | the experimental WebXR version and `serve.py` |
| `tests` | unit tests, and DOS test programs in `tests/dos` |
| `docs` | [GAMES.md](docs/GAMES.md), [FINDINGS.md](docs/FINDINGS.md), [ARCHITECTURE.md](docs/ARCHITECTURE.md) |

[docs/FINDINGS.md](docs/FINDINGS.md) records what was learned about the
hardware and the games: the VIP card's registers, the driver's INT 33h
interface, the stereo layouts, and what each game does.

## License

GPL-2.0-or-later; see [LICENSE](LICENSE). This program is free software: you
can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 2 of the
License, or (at your option) any later version. `third_party/libretro` keeps
its own MIT license.

## Legal

No game binary, ROM, or original Forte driver is included, and none will be.
DOSBox Pure and js-dos are GPL-2; this project's changes to them are applied
by script at build time.
