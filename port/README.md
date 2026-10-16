# Diddy Kong Racing for Nintendo 3DS

A native port of the Diddy Kong Racing decompilation to the Nintendo 3DS. The game code is the decompiled C from
this repository, compiled for ARM. The N64 hardware it expects is provided by a small emulation layer:

- the libultra OS calls, mapped onto threads
- ROM DMA and saves
- an interpreter for the F3DDKR display lists, drawn with the 3DS GPU
- a C version of the RSP audio microcode

Nothing from the game is distributed. The port reads every asset from your own ROM at runtime.

## What you need

- A Diddy Kong Racing ROM. The US 1.0 version (`baserom.us.v77.z64`, SHA1
  `0cb115d8716dbbc2922fda38e533b9fe63bb9670`) is the one that is tested. `.z64`, `.n64` and `.v64` byte orders all
  work.
- To build: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS packages
  (`dkp-pacman -S 3ds-dev`, which includes devkitARM, libctru, citro3d, picasso and 3dsxtool).
- To play: a 3DS running homebrew (the Homebrew Launcher), or the Azahar emulator. A New 3DS is recommended; on a
  New 3DS the renderer runs on its own CPU core.

## Building

The build needs `include/asset_enums.h`, which the decomp's asset tool generates from your ROM:

```sh
cp /path/to/your/rom.z64 baseroms/baserom.us.v77.z64
make -C tools dkr_assets_tool
./tools/dkr_assets_tool extract -dkrv us.v77
make -C port TARGET=3ds -j8
```

The result is `port/build/3ds/dkr.3dsx`.

## Installing

Copy these to the SD card:

| File                          | Goes to                         |
| ----------------------------- | ------------------------------- |
| `port/build/3ds/dkr.3dsx`     | `sdmc:/3ds/dkr.3dsx`            |
| your ROM                      | `sdmc:/3ds/dkr/baserom.z64`     |

Then start it from the Homebrew Launcher. The ROM may have any name: the port also looks for any `.z64`, `.n64` or
`.v64` file in `sdmc:/3ds/dkr/`, `sdmc:/3ds/` and the root of the SD card. If it cannot find one it says so on the
top screen. Saves go to `sdmc:/3ds/dkr/`:
- `eeprom.bin` holds the game's own save.
- `controllerpak.bin` is a virtual Controller Pak for ghosts and copied saves.

Sound needs the DSP firmware dump at `sdmc:/3ds/dspfirm.cdc`, the same file other homebrew uses. Without it the game
runs silently.

## Controls

| 3DS           | N64                 | In game                 |
| ------------- | ------------------- | ----------------------- |
| Circle Pad    | Control stick       | Steer                   |
| A             | A                   | Accelerate              |
| B             | B                   | Brake / reverse         |
| L or ZL       | Z                   | Use item                |
| R or ZR       | R                   | Hop / drift             |
| X             | C-Up                |                         |
| Y             | C-Left              |                         |
| C-Stick       | C buttons           | (New 3DS)               |
| D-Pad         | D-Pad               |                         |
| SELECT        | L                   |                         |
| START         | START               | Pause                   |

The menus are driven by the stick, as on the N64.

## How it works

| Path                       | What it does |
| -------------------------- | ------------ |
| `src/os/`                  | libultra replacement: threads, message queues, events, timers, PI (ROM), SI (controllers, EEPROM, Controller Pak), VI, AI and the RCP. Every N64 thread is a real thread; on the 3DS they all share one core with strict priorities, as on the N64. |
| `src/game/asset_swap.c`    | The ROM is big endian, so each asset is converted to native byte order as it is loaded. |
| `src/game/`                | Other per-port game support: C versions of the few hand-written assembly routines, and zlib inflate. |
| `src/gfx/gfx_rdp.c`        | F3DDKR display list interpreter. Transforms vertices on the CPU, decodes and caches textures, decodes the RDP colour combiner and blender. |
| `3ds/gfx_c3d.c`            | citro3d backend. Compiles the combiner to TEV stages. |
| `host/gfx_soft.c`          | Software rasteriser backend for the host build. |
| `src/audio/audio_rsp.c`    | C implementation of the aspMain audio microcode. The resampler table is read from the ROM. |
| `3ds/`                     | Other 3DS frontend parts: input, ndsp audio output, the main loop. |
| `host/`                    | Other host frontend parts. |

Changes to the game code itself are all under `#ifdef TARGET_PORT` or `#ifdef AVOID_UB`. A handful of original bugs
were harmless on the N64 but corrupt memory with a different layout (out of bounds writes and reads); those are fixed
under `AVOID_UB`.

## Host build (for development)

`make -C port TARGET=host` builds a headless Linux version (32 bit, so `gcc-multilib` is needed). It runs the same
game and interpreter with the software renderer, which makes debugging much easier. It is configured with
environment variables:

| Variable            | Effect |
| ------------------- | ------ |
| `DKR_DATA_DIR`      | Directory with `baserom.z64` and the saves (default: current directory). |
| `DKR_FRAMES`        | Exit after this many frames. |
| `DKR_DUMP_EVERY`    | Write every Nth frame to `DKR_DUMP_DIR/frame_NNNNN.png`. |
| `DKR_INPUT`         | Scripted input, e.g. `"700:START 710: 900:A 910: 1250:STICK=0,-80 1262:"`: each step holds the listed buttons from that poll number until the next step. |
| `DKR_DUMP_AUDIO`    | Write the audio output to this file as raw 16 bit stereo PCM. |
| `DKR_TRACE_FRAME`   | Log every display list command and draw of this frame. |

`make -C port TARGET=host DEBUG=asan BUILD_DIR=build/asan` builds with AddressSanitizer, which is how the original
memory bugs above were found.

## Graphics

The top screen is anti-aliased by default: frames are drawn at twice the resolution in each direction and
downsampled when they are sent to the screen. It only costs GPU time (the CPU work is unchanged). To turn it off, put
`DKR_AA=0` in `sdmc:/3ds/dkr/env.txt`.

In Azahar you can also raise the internal resolution in the graphics settings; the port renders on the 3DS GPU, so
that applies to it as well.

## Debug settings on the 3DS

The start-up is always logged to `sdmc:/3ds/dkr/log.txt`, and its progress is shown on the bottom screen, so if
the game does not start, the last line says how far it got. Create `sdmc:/3ds/dkr/debug` (an empty file) to keep
logging after start-up. Settings go in
`sdmc:/3ds/dkr/env.txt` as `NAME=VALUE` lines. `DKR_TRACE_FRAME` and `DKR_INPUT` (which replaces the real
controls) work there, as do these renderer switches:
- `DKR_NO_CULL=1`
- `DKR_NO_SCISSOR=1`
- `DKR_NO_FOG=1`
- `DKR_VERTEX_FOG=1` (the old per vertex fog, for comparison)
- `DKR_GFX_TEST=1` (draws a reference quad)
- `DKR_SINGLE_CORE=1` (on a New 3DS, keeps the renderer on the main core as on an Old 3DS)

## Automated testing

These settings work on both builds (in `env.txt` on the 3DS, as environment variables on the host):

| Setting                  | Effect |
| ------------------------ | ------ |
| `DKR_WARP=all` or `DKR_WARP=4,7,12` | The title screen's attract demos load these levels instead (ids in `ASSET_LEVEL_*` order), one after another, with the AI driving and the title overlay hidden. |
| `DKR_WARP_SECONDS=n`     | How long each warped level runs (default 20). |
| `DKR_DIFF_EVERY=n`       | 3DS only: every nth frame is also drawn by the software renderer, and both images are written to `sdmc:/3ds/dkr/` (`diff_N_ref.png`, `diff_N_gpu.bin`). `port/tools/diff_frames.py <dir> <out>` compares them and writes side by side images of the frames that differ. |
| `DKR_TEST_SAVE=1`        | Starting a save file gives it complete progress: all balloons, keys, trophies, amulets and Taj challenges, and every boss and door. The progress is written to the save, so the file keeps it without the setting. |

`port/tools/testdata/eeprom_complete.bin` is a save made this way (file A, "AAA"). Copying it over `eeprom.bin`
replaces your own saves.

## Testing in Azahar

The port runs in the Azahar emulator.

1. Open the emulator's SD card folder with *File > Open Azahar Folder*, then go into `sdmc`. It is usually
   `%APPDATA%\Azahar\sdmc` on Windows, `~/.local/share/azahar-emu/sdmc` on Linux or
   `~/Library/Application Support/Azahar/sdmc` on macOS.
2. Put the ROM in `3ds/dkr/` inside that folder.
3. Load `dkr.3dsx` with *File > Load File*.

For sound in Azahar, put any file at `sdmc/3ds/dspfirm.cdc`; Azahar's emulated DSP does not need the real firmware.

## Status

- **Tested in Azahar:** the intro, attract mode and menus, the adventure hub and races.
- **Performance in Azahar:** races run at the game's 30 fps cap in both Old 3DS and New 3DS mode, with time to
  spare (`DKR_PROFILE=1` in `env.txt` logs the frame rate, graphics and audio time, and idle time).
- **Untested:** real hardware. Azahar does not model CPU timing exactly, so expect the real frame rate to be lower,
  especially on an Old 3DS.
- **Known gaps:**
  - Only the US 1.0 ROM is tested.
  - Rumble isn't supported.
  - The bottom screen is unused.
