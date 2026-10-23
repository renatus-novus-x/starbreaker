[English](README.en.md) | [日本語](README.md)

# STAR BREAKER for X68000

![STAR BREAKER teaser](images/teaser.png)

**[Launch STAR BREAKER in WebX68k](https://uraraworks.github.io/WebX68k/?cpu=10&fd1=https://raw.githubusercontent.com/renatus-novus-x/starbreaker/main/dist/starbreaker.xdf&run=1)**

An original brick-breaking game for the Sharp X68000, written in C++
with the elf2x68k cross toolchain (m68k-xelf-gcc/g++). It boots
standalone — **no Human68k required**: the game itself is the system
file (`breakout.sys`, built hellosys-style), placed on the disk as
`HUMAN.SYS` so the X68000 IPL loads it directly.

The full Japanese README is in [README.md](README.md).

## Features

- 512×480, 65536 colors (CRTMOD 12), CRT controller retimed in the
  vertical blank to ≈60 Hz; frame pacing locked to the VDISP signal
  (MFP GPIP) for flicker-free play — about 0.7 ms of game logic and
  drawing per frame even at 10 MHz
- Ball and paddle are hardware sprites; the HUD lives on the text
  plane; block erasing and background restore use direct GVRAM writes
- YM2151 (OPM) sound driven directly via IOCS: a futuristic A-minor
  synthwave BGM loop (sequencer arpeggios + octave-pulsing bass),
  sound effects and jingles on separate channels
- Attract mode: leave the title screen alone for about 5 seconds and
  an AI demo game starts (any key returns to the title; demo scores
  never touch the hi-score)
- 8×8 dot font: "8x8 ORIGINAL FONT 1 fix" from the MSXPen shared code
  (https://msxpen.com/codes/-Nq8q6wabU6mJ4onDmYE), glyphs for ASCII
  0x20–0x7F
- Keyboard and joystick/joypad (port 1) support

## Controls

| Input | Action |
|---|---|
| Cursor Left/Right, A/D, Z/X, joypad | Move paddle |
| SPACE / pad button | Start game, launch ball, retry |
| P | Pause |
| M | Sound on/off |
| ESC | Back to title (on title: exit) |

## Building

Requirements: the elf2x68k toolchain (`m68k-xelf-g++`) on your PATH,
plus Python 3.

```
make            # breakout.sys (bootable system) + breakout.x (Human68k .X)
```

A ready-to-boot disk image is shipped in `dist/starbreaker.xdf`, so
building the image yourself is not required. (Regenerating it takes a
separate script that uses a Human68k 3.02 disk image as a template;
that script is not part of this repository.)

## Running

- Emulator: load `dist/starbreaker.xdf` (2HD, 1,261,568 bytes) as
  FDD0 and boot. In [WebX68k](https://uraraworks.github.io/WebX68k/),
  insert the image and use the "boot with the inserted disk" button,
  or use the launch link at the top of this page.
- Real hardware: write the image to a 2HD floppy and boot from it.

## Files

| File | Description |
|---|---|
| `breakout.cpp` | The whole game (single-file C++) |
| `Makefile` | Build rules (see above) |
| `dist/starbreaker.xdf` | Ready-to-boot disk image (the game as HUMAN.SYS) |
| `images/teaser.png` | Title screen screenshot |
| `README.md` | Japanese README with design and optimization notes |
