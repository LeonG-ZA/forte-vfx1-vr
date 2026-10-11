# Games: settings that make them work

No game files come with this project. Each game is a zip you make from your
own copy, with a `DOSBOX.CONF` inside that starts it with its VR switches
(see "Making a game zip" in the [README](../README.md)).

Every VFX1 game needs the card's port in its environment:

```
SET VIPPORT=260 I5
```

The launcher's per-game settings (menu button, then **Settings**, with the
game running) are saved per zip. The ones that matter here:

| Setting | Values | When to change it |
| --- | --- | --- |
| Stereo | off, VFX1 pages, interlaced lines | the headset mode the game draws for |
| Stereo or flat screen | fixed, detected (experimental) | fixed is reliable; detected switches to flat for menus by itself |
| Always show | stereo, flat | with fixed |
| Mode change keymap | F9 or left Y, Scroll Lock or left stick click, Pause, none | steps auto, stereo and mono while playing |
| Head movement | looks (1:1), steers | steers for games that turn by head movement, like Descent |
| Head tracker on a COM port | as the zip sets, i-glasses! COM1/COM2, CyberMaxx COM1/COM2 | i-glasses! and CyberMaxx modes |
| Mouse | off, CyberPuck tilt, point right, point left | games that need a mouse pointer |

**Default stereo layout from the zip name.** A game not yet set starts in
**interlaced lines** when its zip name contains `iglass`, `i-glass`, `maxx`,
`interlace` or `lcd`, and in **VFX1 pages** otherwise. Naming a zip
`quake-lcd.zip` or `descent2-iglasses.zip` therefore needs no setting.

## Per game

### Descent (shareware 1.4)

VFX1 pages, head tracking.

```
SET VIPPORT=260 I5
CD DESCENT
dos32a DCNTSHR.EXE -vfx -vfxtrak
```

Set **Head movement: steers**. Enter in flight zeroes the headset and
toggles the HUD; Shift+F7 swaps the eyes, Shift+F6 changes the resolution.
The HUD and crosshair are drawn in the left eye only, as on a real VFX1.

### Descent 2 (demo)

Three headsets, one switch each. Set **Head movement: steers**.

| Mode | Switches | DOSBOX.CONF extra | Stereo |
| --- | --- | --- | --- |
| VFX1 | `-vfx -vfxtrack` | | VFX1 pages |
| i-glasses! | `-iglasses -viotrack 2` | `[serial]` `serial2=iglasses`, `[joystick]` `joysticktype=none` | interlaced lines |
| CyberMaxx | `-cybermaxx -maxxtrack 2` | `[serial]` `serial2=cybermaxx`, `[joystick]` `joysticktype=none` | interlaced lines |

### Quake 1.08

Interlaced stereo through Quake's LCD support, with VFX1 head tracking
through the VFX1QK loader:

```
VFX1QK QUAKE.EXE -nocdaudio +vid_mode 7 +lcd_x 1
```

Put `lcd` in the zip name, or set Stereo to interlaced lines.

### Z.A.R.

Use the game's **i-glasses!** stereo mode (VFX1 stereo shows one view, see
[FINDINGS.md](FINDINGS.md) Q15). Set the head tracker to i-glasses! on the
port the game uses, `joysticktype=none`, Stereo to interlaced lines, and bind
the CyberPuck in the game's Customize Controls.

### MadSpace

VFX1 pages, head tracking. Needs a fixed speed:

```
[cpu]
cycles=77000
```

### Gridfighter 3D

VFX1 pages. Run its mode setter first:

```
SETMODE.COM
GRID3D.EXE
```

### System Shock (CD)

Start `CDSHOCK.EXE`, turn on VFX1 stereo in the game's options and use its
full-screen view. The mouse cursor and menus are in one eye only.

### Magic Carpet / Magic Carpet Plus

`-vfx` gives head pitch and roll; no stereo (Q17).

### Magic Carpet 2

`-level 1 -skipscreens`. No head tracking or stereo found.

### MechWarrior 2

Head tracking only: select VFX-1 under Cockpit Controls in the clan hall's
Esc menu. Stereo needs a file the copies tried lacked (Q18).

### Not working

Comanche 3 (no stereo), Stormfighter shareware (no VFX1 support), 3D Stereo
Set titles (not emulated).
