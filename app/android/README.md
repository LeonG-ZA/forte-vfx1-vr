# VFX1 Quest: the native OpenXR app

A sideloaded Quest 3 app that runs DOS in DOSBox Pure, with the emulated
Forte VFX1, in OpenXR. See docs/ARCHITECTURE.md for how it shares code with the
WebXR page.

- [src/main_android.c](src/main_android.c) binds Android (NativeActivity, EGL,
  AAudio, logcat) to the OpenXR app shared with Windows, [../xr/xrapp.c](../xr/xrapp.c)
  (see ../windows/README.md). That in turn binds OpenXR and OpenGL ES to the
  shared C: retrohost (the libretro frontend), vfxhost (head and puck angles,
  page-flip stereo, the controller layout) and, inside the core, libvfx1dev.
- The display is head-locked at the real VFX1's image size, 35.5 degrees wide
  at 3.35 m, with one eye image per eye when the title draws VFX1 stereo.
- Controls and settings are described in the [main README](../../README.md).

## Build

In WSL, with the Android NDK r27c, the OpenXR loader and headers under
~/android and DOSBox Pure at ~/dosbox-pure:

```bash
bash app/android/build.sh
```

Then on Windows (Git Bash), with the Android SDK's build tools 34 and
platform 34, and Java 17:

```bash
bash app/android/package.sh
```

That writes build/android/vfx1quest.apk, signed with a local debug key.

## Install without adb

build.sh can bake in a game URL the app downloads on first launch, when its
files directory has no zip yet:

```bash
VFX1_CONTENT_URL=http://<pc-address>:8080/mygame.zip bash app/android/build.sh
```

Serve a folder holding the APK and the zips with `web/serve.py --dir folder
--lan`, and download and install the APK in the Quest's browser (the server
sends the Android package MIME type for .apk files). Build with
`VFX1_SERVER_DIR=http://<pc-address>:8080/` to get the launcher's
"Download games from the PC..." page.

## Install and run with adb

1. Enable developer mode for the Quest 3 in the Meta Horizon phone app, and
   connect it to the PC with USB. Allow USB debugging in the headset.
2. Install:
   ```bash
   adb install -r build/android/vfx1quest.apk
   ```
3. Push a game zip to the app's files directory. The app loads the first .zip
   there first; the launcher lists the rest. See docs/GAMES.md for each game's
   DOSBOX.CONF:
   ```bash
   adb shell mkdir -p /sdcard/Android/data/com.fortevfx.vfx1quest/files
   adb push mygame.zip /sdcard/Android/data/com.fortevfx.vfx1quest/files/
   ```
4. Launch "VFX1 Quest" from Unknown Sources in the app library.
5. Logs:
   ```bash
   adb logcat -s VFX1Quest DOSBoxPure
   ```

Game files are never committed to this repository.
