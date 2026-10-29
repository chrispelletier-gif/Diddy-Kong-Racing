# Diddy Kong Racing for Nintendo 3DS

An unofficial, native Nintendo 3DS version of Diddy Kong Racing, built from the community decompilation of the N64
game. It isn't an emulator: the game's own code runs on the 3DS, drawn by the 3DS GPU, with widescreen, stereo 3D and
a race HUD on the bottom screen.

**This is an early alpha (v0.1) and still a work in progress.** It plays well on a New 3DS, but not every part of
Adventure mode has been played through yet, and it hasn't been tried on an original 3DS or 2DS. Please report what you find (see [Reporting a problem](#reporting-a-problem)).

**No game files are included.** You need your own copy of the game: the port reads everything (models, textures,
music, text) from your ROM when it starts.

## What you need

- **A New Nintendo 3DS, New 3DS XL or New 2DS XL** with homebrew (the Homebrew Launcher, or custom firmware for the
  .cia). The original 3DS, 3DS XL and 2DS haven't been tested yet: they have a slower processor and less memory,
  so the game may run slowly or not start. If you try one, please say how it went in an issue.
- **A Diddy Kong Racing ROM, US version 1.0** (SHA1 `0cb115d8716dbbc2922fda38e533b9fe63bb9670`). `.z64`, `.n64` and
  `.v64` files all work. Other versions (US 1.1, PAL, Japan) will be supported in a later release; the game tells you
  if your ROM is a different version.
- **For sound:** the DSP firmware dump at `sdmc:/3ds/dspfirm.cdc`, the same file other homebrew uses (made with
  [DSP1](https://github.com/zoogie/DSP1)). Without it the game runs silently.

## Installing

1. Download `dkr.3dsx` or `dkr.cia` from the [latest release](../../releases).
2. Put your ROM on the SD card as `sdmc:/3ds/dkr/baserom.z64`. (Any name works if it's in `sdmc:/3ds/dkr/`,
   `sdmc:/3ds/` or the root of the SD card.)
3. Either:
   - copy `dkr.3dsx` to `sdmc:/3ds/` and start it from the Homebrew Launcher, or
   - install `dkr.cia` with FBI (or another installer) and start it from the HOME menu.

The first start takes a little longer while the game checks the 3DS GPU once.

### Optional: boss and Taj portraits

The bottom-screen race HUD shows a portrait for every racer. The ROM only has portraits of the ten playable
characters, and we can't ship Rare's artwork, so the bosses and Taj get a box with their initial unless you add
portraits yourself: six 40x40 PNG files in `sdmc:/3ds/dkr/portraits/` named `tricky.png`, `bluey.png`, `bubbler.png`,
`smokey.png`, `wizpig.png` and `taj.png`. Any other size or format is skipped.

## Playing

| 3DS | Does |
| --- | --- |
| Circle Pad | Steer |
| A | Accelerate |
| B | Brake / reverse |
| L | Use item (N64 Z) |
| R or ZR | Hop / drift |
| ZL | Look behind |
| X / Y | C-Up / C-Left |
| C-Stick (New 3DS) | C buttons |
| D-Pad | D-Pad |
| SELECT | N64 L |
| START | Pause |

Buttons can be remapped, and the circle pad's deadzone, sensitivity and response curve changed, in **Settings >
Controls**.

**Settings** is the button at the bottom of the touch screen, and the same settings are also in the game's own Options
menu (INTRO LOGOS, and VIDEO, SCREEN, CAMERA and CONTROL OPTIONS) and the pause menu, in English or French as the game
is set. What you can change:
- **Picture:** anti-aliasing (4x, 2x or off), 30 or 60 fps, mipmaps, colour boost.
- **Screen:** widescreen (Wide, Balanced or Zoomed), the sharper 800-pixel mode, stereo 3D (follows the 3D slider),
  HUD at the screen edges or in the 4:3 area.
- **Race HUD:** on the bottom screen (standings, map, item, lap times), on both screens, or the N64's HUD on top.
- **Camera:** Modern (calmer, the default) or the original N64 camera, and whether the camera stops in front of walls.
- **Sound:** smooth resampling.
- **Intro logos:** shown, or skipped straight to the title screen. A, B or START also skips them.
- **Saves:** back up and restore your save (below).

60 fps works best with anti-aliasing off or at 2x; a few heavy levels drop back to 30 fps with 2x on.

Your saves are in `sdmc:/3ds/dkr/`: `dkr.eep` is the game's save and `dkr.mpk` the Controller Pak with your ghosts,
and your settings are in `settings.txt` and `controls.txt` next to them. The save files are the same as an N64
emulator's (ares, Project64, mupen64plus; US version), so you can bring a save over or take yours to an emulator by
copying and renaming the file. **Settings > Saves** backs up your save and ghosts to a dated folder in
`sdmc:/3ds/dkr/backup/` and restores one; to restore an emulator's save, put its `.eep` (and `.mpk`, if any) in a
folder of its own there and pick it. A restore keeps a copy of the save it replaces and closes the game, which loads
the restored save next time. Saves from earlier builds (`eeprom.bin`, `controllerpak.bin`) are converted the first
time a newer build starts.

The game is one player only: a 3DS has one set of controls.

## Known issues

- Only the US 1.0 ROM works for now.
- Not yet tested on the original 3DS, 3DS XL or 2DS.
- Boss and Taj portraits need the optional files above.

## Reporting a problem

If the game crashes it writes `sdmc:/3ds/dkr/crash.txt` and its log to `log.txt`; once you start the game again
they become `last_crash.txt` and `last_log.txt`. Please [open an issue](../../issues/new) saying what you were doing,
which console you have and which release you downloaded, and attach those two files (from `sdmc:/3ds/dkr/`).

## Building it yourself

See [port/README.md](port/README.md): it covers the build, the debug build's tools, how the port works and how it is
tested. The N64 decompilation this is built on is described in [doc/decompilation.md](doc/decompilation.md).

## Credits and legal

- The Diddy Kong Racing decompilation project and its contributors, whose work this port is built on.
- [devkitPro](https://devkitpro.org/), libctru and citro3d, and the other libraries listed in [NOTICE.md](NOTICE.md).

Much of the 3DS port's code was written with AI assistance ([Claude Code](https://claude.com/claude-code)).

This project is not affiliated with or endorsed by Nintendo or Rare. Diddy Kong Racing and its characters are
trademarks of their respective owners. No game assets are distributed here; you need your own legally obtained copy
of the game. The 3DS port is under the MIT License and the decompilation under CC0 (see [LICENSE.md](LICENSE.md)); the libraries
in the built game have their own licences, listed in [NOTICE.md](NOTICE.md).
