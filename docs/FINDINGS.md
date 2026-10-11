# Findings

What was learned about the Forte VFX1, its rivals and the games that used
them, while emulating them. Each entry says what is known, how it was found,
and what is still open. Code comments refer to these entries by number, so
the numbers have gaps where older entries were retired.

Sources used, none of them in this repository: Forte's VFX1 SDK 2.13 headers
and examples (VFX1.H, VFX1REGS.C, EXAMPLE1.C), Forte's driver VFX1.COM 2.14,
Parallax's released Descent and Descent 2 source, Forte's official VFX1 FAQ,
and the games themselves, logged and disassembled while running.

---

## Q1 - VIP card I/O ports and registers

**Known well enough for every title tried.**

- **Base port** comes from the `VIPPORT` environment variable, for example
  `SET VIPPORT=260 I5`. Forte's installer offered 260, 280, 2A0 and 2C0, and
  IRQ 5, 7, 10, 11, 12 or 15. Games parse the port themselves. The emulated
  card answers at 260h and also at 300h, where some titles look without
  reading VIPPORT.
- **Port offsets**: 0 address index (AIR), 1 register data (CSR), 2 and 4 data
  FIFOs, 8 and 9 ACCESS.bus control and data, A IRQ config, C hardware id,
  D and E hardware version, F config status.
- **Indexed registers** through AIR and CSR: 0 SCR command, 1 SSR status,
  2 ICR interrupt control, 3 ISR interrupt status, 4 PSR page select, 5 VRR
  ready, 6 PCR palette control, 7 PPMR palette mask, 8 PRAR palette read
  address, 9 PWAR palette write address, 10 PCRR palette data.
- **PSR** is the one that matters for stereo: high nibble left-eye page, low
  nibble right-eye page.
- **Head tracking and the CyberPuck are not on the card's ports.** Games call
  the Forte driver, VFX1.COM, through INT 33h (Q13).

## Q2 - Do titles write to the VIP card, or only poll?

**They write; nothing tried uses the card's interrupt.**

Descent's released source writes PSR once per frame to flip pages, downloads
the palette through PCR, PPMR, PWAR and 768 PCRR bytes, and switches the card
between stereo and mono. Nothing reads a card register or handles an
interrupt, so the emulated card needs write handlers and PSR state only.

The shareware Descent 1.4 executable writes nothing to the card at all, yet
draws page-flipped stereo. The app therefore cannot rely on PSR to know which
page is complete, and finds it from the pixels (Q3).

## Q3 - The VFX1's stereo layouts

**Known: the card has three modes.**

Forte's set-video-mode call documents 0 mono, 1 multiple-buffer stereo and
2 line-sequential stereo. Multiple-buffer, which most VFX1 titles use:

- The 640x480 VGA frame holds four 320x240 pages. Left-eye pages are the left
  half, right-eye pages the right half, and each exists on the even and on the
  odd scanlines.
- The game draws the hidden parity and then writes PSR to show it, so a VFX1
  never showed a half-drawn page.
- Descent renders 160x120 per eye in its default setting and doubles it both
  ways. Shift+F6 in game cycles to higher resolutions.

The app assembles pages from the pixels: a page is complete once both of its
halves have been drawn, a change of palette alone is not a new page, and tiny
changes such as a moving mouse cursor are ignored.

### Q3b - Which half is the left eye?

The left half. Descent's Shift+F7 swaps them in game.

## Q4 - Per-eye field of view

**Known, from Forte's official VFX1 FAQ.**

Each eye's image spanned 35.5 degrees horizontally, 26.4 vertically and 45
diagonally, which Forte described as a 120 inch screen seen from 11 feet. The
45 degree figure often quoted is the diagonal. The apps show the VFX1 picture
at 35.5 degrees wide and 3.35 m away by default.

Source: https://mellottsvrpage.com/vfxfaq.txt

## Q7 - Which way round does a title want the mouse axes?

**Per title.** The mouse path (used only by the experimental WebXR page for
titles without VFX1 support) turning the head left emits negative mouse X, and
looking up negative mouse Y. Both can be inverted in configuration.

## Q12 - How Descent turns the ship from head movement

**Measured.** Descent's native VFX1 tracking converts each frame's change in
head angle into turn rate, exactly as it does mouse motion
(main/kconfig.c read_head_tracker): the ship turns, not the view. Each frame's
heading input is also clamped (main/arcade.c), so very fast head turns
under-rotate. Measured from the game's own pixels, the camera starts moving
about 85 ms after the head and eases in with a time constant of roughly
130 ms. This is why the apps keep the picture head-locked rather than
re-projecting it into the world, and why the launcher offers "Head movement:
steers" as a per-game setting.

## Q13 - The Forte driver's INT 33h "Sense" interface

**Implemented in src/vfx1dev, matching VFX1.COM 2.14 closely enough for every
title tried.**

Games find VFX1.COM by calling INT 33h with AH from 60h to 70h and AL = 7Fh.
The driver answers AX = 7F00h plus that AH, and AH becomes the function prefix
for every later call, with the function in AL. Mouse calls never use those AH
values, so the interface shares INT 33h with the mouse driver.

| AL | Function | Registers |
| --- | --- | --- |
| 00h | reset device | |
| 01h, 02h | get and set data sizes | |
| 03h, 04h | get and set configuration packets | ES:DX buffer, CX bytes |
| 05h, 06h | report, continue | |
| 07h, 08h | reset zero, set zero | BX = 1 + class * 256 |
| 09h | test devices | |
| 70h | get device data | in BX = 1 + class * 256; out BX yaw, CX pitch, DX roll, AL buttons, AH error |
| 71h | set video mode | CX = 0 mono, 1 multiple-buffer, 2 line-sequential |
| 7Fh | driver status | detection, above; BX = 0, CX = (CX + 1) or 100h as a handshake |
| 80h | set video page | CX = page, as the page select register |
| 90h | set callback | |

Device classes are 2 head-mounted system, 3 head tracker, 4 CyberPuck. Every
device is number 1. The configuration packets are SYSI (system name "SENS"),
VNDI, HMS (bit 0 set: attached), HTD, HPD and NULL. Z.A.R. checks all of
these; Descent only detects the driver, reads the head tracker and zeroes it.

Angles are signed 16-bit, 65536 to a full turn (Forte's Win95 SDK: 0 to 65535
spans -179 to 180 degrees). Positive yaw is left, positive pitch is up,
positive roll tilts right. The CyberPuck reports pitch and roll tilt and three
buttons; it is taken from the controller's rotation relative to where it was
when the game started, so holding it at any comfortable angle is level.

## Q14 - The i-glasses! and CyberMaxx: stereo format and head trackers

**Implemented from Descent 2's code; angle scales chosen, not known.**

Both rivals took line-interlaced stereo from the VGA output: even scanlines
the left eye, odd the right. Descent 2 draws 320x400 for the i-glasses! and
the CyberMaxx 2, and 320x200 (100 lines an eye) for the first CyberMaxx.
Quake's `lcd_x` draws the same layout.

Their head trackers sat on a COM port. The protocols (include/vfx/vrtrack.h)
are from Parallax's released source: main/iglasses.c in Descent 2 and
main/victor.c in Descent. DOSBox gets serial port types `iglasses` and
`cybermaxx`.

Each device's angle units are not known. vrtrack scales every angle so that
Descent 2 turns exactly as far as it does for the VFX1.

Two emulation traps, both handled:

- Descent 2 waits 10 ms for a packet and asks again if it is late. A request
  mid-packet replaces the unfinished packet, and bytes go out as fast as the
  UART takes them.
- With a serial tracker running, Descent slowly rolled and turned the ship
  with the head still. The cause was the DOS joystick: Descent times the game
  port with a loop, and the tracker's serial interrupts landing in it read as
  a tilted stick. Game zips that use a serial tracker set
  `joysticktype=none`.

## Q15 - Z.A.R.'s VFX1 stereo

**Open. Z.A.R. detects the emulated VFX1 and reads the head tracker; in VFX1
Stereo 320x240 it shows one view. The demo (1.02) and the full version (1997)
behave alike. Its i-glasses! stereo mode works.**

What the game does in VFX1 Stereo 320x240 (logged and disassembled):

- It sets video mode 2 (line-sequential) and set video page with CX = 01h.
  Its set-up routine also has a stereo branch (CX = 203h, page select 23h
  written straight to the card), but its only caller asks for the mono branch.
- It switches to a 640x480 VESA screen with a normal 640-pixel line.
- Its stereo mode installs routines that render each eye and copy them side
  by side on alternate lines, but in play only the left half of the even
  lines is ever drawn, and at maximum interaxial successive pictures show no
  disparity. What chooses the one-view path was not found; the mode-list
  checks pass.

A period account of Z.A.R. running in stereo on a real VFX1 would say whether
this is the game or the emulation.

## Q16 - How did the VFX1 know a picture was mono or stereo?

**Known: the software told it.**

The headset never looked at the picture. Through the driver's set-video-mode
call (71h) a title picks mono, multiple-buffer or line-sequential, and
page-flipping titles write the page select register to choose each eye's
page. Going back to mono for menus was the title's job; the only manual
control was VFX1.COM's `+t`, which forced line-sequential on.

Not every title sends these signals. Neither Descent sets a video mode or
writes the page select register: its stereo is told apart from its menus only
by the screen size and by its tracker reads. Quake's `lcd_x` and the
i-glasses! titles send nothing at all.

So the apps default to **fixed** stereo: the picture is always split the way
the game's stereo setting says, and the mode-change key (F9 or the left
controller's Y by default) steps between auto, stereo and mono at any time.
**Detected (experimental)** instead decides per frame from the title's
signals and, where there are none, from the picture: two halves that are
nearly the same picture, offset sideways, are a stereo pair.

## Q17 - Magic Carpet's VFX1 stereo

**Open: no working VFX1 stereo path was found.** Investigated on Magic Carpet
Plus and the shareware demo.

- `-vfx` starts the driver: head pitch and roll are read (roll steers, pitch
  looks); head yaw and the puck's tilt are read but never used.
- Alt+H toggles the game's VFX1 display flag; F10 steps its stereo modes:
  off, two-eye, autostereogram.
- The two-eye mode merges the eyes through lookup tables that are filled only
  when the VFX1 flag is off (the red/cyan anaglyph). With it on they stay
  empty and the screen is black, or frozen at 640x480. None of this depends
  on what the VFX1 answers.
- In 640x480 the eyes are rendered side by side on the even lines, and the
  game writes the card's control register at the hard-coded ports 302h/303h.
  It never writes the page select register or sets a VFX1 video mode.

The code looks unfinished. The game is not patched.

## Q18 - Other titles

- **MechWarrior 2** (Activision, DOS). VFX1 head tracking works once VFX-1 is
  selected under Cockpit Controls (the clan hall's Esc menu). Its stereo
  needs `VFX\VFXSCAN.DLL`, which the copies tried did not include; it is
  turned on with the `-VG` switch or a flag in MW2SND.CFG.
- **System Shock** (CD version, CDSHOCK.EXE). Stereo works with the game's
  VFX1 option and its full-screen view. The mouse cursor and the menus are
  drawn in one eye only, as the game does.
- **Stormfighter**. The shareware release has no VFX1 support.
- **Comanche 3**. Stereo was not made to work.
- **3D Stereo Set** titles (320x200 shutter-glasses stereo). Not emulated;
  they detect the glasses through INT 10h AX = 4ED0h expecting 3344h.

---

# Legal

Nothing here bundles, embeds or reproduces any game binary, ROM, or original
Forte driver. The reverse-engineering work operates on binaries from legally
obtained copies.
