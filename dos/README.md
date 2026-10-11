# DOS programs

DOS programs that run inside the emulator next to the games. They are
written from scratch, from published sources only.

## VFX1QK.COM: VFX1 head tracking for Quake

[vfx1qk.asm](vfx1qk.asm) stands in for Forte's VFX1QKE.EXE, which we don't
have. It is based only on:

- Quake's external control interface, from `WinQuake/in_dos.c` in id's
  released source (`externControl_t`, `IN_ExternalMove`)
- Forte's VFX1 SDK, for the driver calls

### What it does

1. Finds the VFX1 driver by scanning for the function prefix with function
   7Fh, as `VFX1REGS.C` does.
2. Sets the headset to line-sequential video, the mode the VIP card used for
   interlaced stereo (function 71h, mode 2).
3. Hooks a free interrupt (60h–66h).
4. Runs Quake with `-control <address>` added.

Quake then calls it once a frame. Each time, it:

- reads the head tracker (function 70h, class 3);
- adds the change in yaw, pitch and roll since the last frame to Quake's view
  angles, so mouse and keyboard turning still work;
- sets `AUX_FLAG_FREELOOK`, so Quake doesn't pull the pitch back to level.

### Usage

```
VFX1QK [program [arguments]]
```

The program defaults to `QUAKE.EXE`. For VFX1 stereo and head tracking with
Quake 1.08:

```
SET VIPPORT=260 I5
VFX1QK QUAKE.EXE -nocdaudio +vid_mode 7 +lcd_x 1
```

- **`+vid_mode 7`** is 320x400.
- **`+lcd_x 1`** turns on Quake's interlaced stereo. Each eye moves 1 unit to
  its side, so the eyes are 2 units apart: about 6 cm at Quake's scale.
- **Quake version:** only 1.08 has `lcd_x`. `-control` is in every version.
- **The app:** treats the game as interlaced stereo when its zip name
  contains `lcd`.

### Checked headless (2026-10-03)

With Quake 1.08 started on E1M1:

- Quake calls the driver every frame.
- With the head still, the view doesn't move.
- Holding the head 30 degrees left, 20 degrees up or 20 degrees tilted right
  turns, pitches and rolls the view the same way.

### Build

```bash
nasm -f bin -o VFX1QK.COM dos/vfx1qk.asm
```
