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

The menus are driven by the stick, as on the N64. At the bottom of the bottom screen are three buttons: Bug report,
Test run and Updates (see below). The rest of the touch screen does nothing, so a stray tap can't interrupt a race;
holding SELECT while touching anywhere also sends a bug report.

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

## Bug reports

The 3DS build can send a bug report over Wi-Fi as an issue on this repository, labelled `3ds-bug-report`:
- Tap Bug report at the bottom of the bottom screen, or hold SELECT and touch the screen anywhere. The game saves a
  snapshot of the frame and sends it with the log. This also works when the game has frozen, without the snapshot
  if the renderer has stopped.
- If the game crashes it writes `sdmc:/3ds/dkr/crash.txt` (the registers, the stack and the recent log) and goes
  back to the Homebrew Launcher. The next time it starts, and also after a freeze that needed the power button, it
  offers to send the last run's log and crash report (A sends, B skips).

It needs a GitHub token that may only create issues, in `sdmc:/3ds/dkr/bugreport.txt`:
1. On GitHub open Settings, Developer settings, Personal access tokens, Fine-grained tokens, and choose
   Generate new token.
2. Name it (for example "DKR 3DS bug reports") and pick an expiry date.
3. Under Repository access choose Only select repositories, and pick this repository.
4. Under Permissions, Repository permissions, set Issues to Read and write. Leave everything else as it is.
5. Generate the token and copy it (it starts with `github_pat_`).
6. Create `sdmc:/3ds/dkr/bugreport.txt` containing one line: `token=github_pat_...`. A `repo=owner/name` line sends
   the reports to another repository.

The token never leaves the console except to GitHub, and is not part of the reports. A report that cannot be sent
is kept as `sdmc:/3ds/dkr/bugreport_unsent.txt`.

The issue body has the build, the console, the counters and the end of the log. The files (the snapshot's
`diff_N_ref.png`, `diff_N_gpu.bin` and `diff_N_log.txt`, `log.txt`, `env.txt`, or `last_log.txt` and
`last_crash.txt` after a crash) are posted as compressed comments on the issue. To get them back:

```sh
python3 port/tools/bugreport_fetch.py --list        # open reports
python3 port/tools/bugreport_fetch.py 12             # saves issue 12 to bugreport_12/
python3 port/tools/diff_frames.py bugreport_12 bugreport_12/diff
```

A crash report gives `pc` and `lr`; with the `dkr.elf` of the same build (the `Build` line is its commit),
`arm-none-eabi-addr2line -f -e dkr.elf <pc> <lr>` names the functions.

## Updates

CI publishes every 3DS build as a pre-release: `3ds-newest` is the latest push to any branch, `3ds-master` the
latest build of master. Tap Updates at the bottom of the bottom screen; the game lists both builds (the
one running is marked "this") and offers master, newest or cancel. The chosen build is downloaded next to the
running `.3dsx`, checked against its size and sha256, and replaces it; tap Quit to finish the update, or keep
playing and the new build runs next time. It only works when the game was started from the SD card (not over
3dslink).

While the repository is public no token is needed. For a private repository the token in
`sdmc:/3ds/dkr/bugreport.txt` is used, and it needs Contents set to Read-only as well (step 4 above).

## Profiler

`DKR_PROFILE=1` in `env.txt` (and the test run below) shows a profiler on the bottom screen, updated every second:

| Line | Meaning |
| ---- | ------- |
| `PROFILER 29.8 fps limit: GPU` | Frames per second, and which part limits them: the game CPU, the renderer CPU or the GPU. |
| `30fps 97% 20fps 3% slower 0%` | Share of frames shown at 30 fps, at 20 fps, and slower. |
| `worst frame 61 ms hitches 2` | The longest frame, and frames over 50 ms. |
| `game logic / scene / wait` | Game thread, ms per frame: object updates (racers, AI), building the display list, waiting for the renderer. |
| `rend rdp / tex / gpuwait` | Renderer thread: interpreting the display list and sending it to the GPU, decoding textures, waiting for the GPU. |
| `gpu draw / audio / idle` | The GPU's drawing time, the audio mixing, and how much of the main core is idle. |
| `tris / draws / tex+` | Triangles and draw batches per frame, textures decoded in the last second. |

On a New 3DS the game thread, the renderer and the GPU work in parallel, so whichever of `logic + scene`,
`rdp + tex` and `gpu draw` is largest sets the frame rate (33.3 ms per frame is 30 fps). Every few seconds a summary
line also goes to the log.

## Unattended test run

Tap Test run at the bottom of the bottom screen, then Start test, and leave the console on its charger. From the
title screen, the game races every track with every character (200 races, some 8 hours), with the AI driving your
kart. Each race is profiled and checked: a race that doesn't finish within 5 minutes, a kart that stops reaching
checkpoints for a minute, a crash or a freeze are all recorded. The bottom screen shows the profiler, the race, and a
progress bar. Your saves are not written while it runs. Tap Stop test to end it early.

- Results go to `sdmc:/3ds/dkr/`: `autorun_results.csv` (one line per race: outcome, frame rate, the profiler's
  stages, worst frames) and `autorun_problems.txt`.
- When the run ends or is stopped, and bug reports are set up, the results are sent as an issue titled
  "[3DS test run]". `python3 port/tools/bugreport_fetch.py <issue>` saves its files, and
  `python3 port/tools/autorun_report.py bugreport_<issue>/autorun_results.csv` turns them into tables by track and
  by character, the slowest races and the problems.
- If the game crashes or freezes, start it again: it sends the crash report without asking and carries on with the
  next race. (A freeze is noticed when nothing has been drawn for 20 seconds.)

`env.txt` settings change what it runs (the same settings work on the host build):

| Setting | Effect |
| ------- | ------ |
| `DKR_AUTORUN=1` | Run at every start, without the button. |
| `DKR_AUTORUN_TRACKS=5,3` | Only these tracks (`ASSET_LEVEL_*` ids; default: the 20 of the tracks menu). |
| `DKR_AUTORUN_CHARACTERS=3,9` | Only these characters: Krunch 0, Bumper 1, Tiptup 2, Conker 3, Timber 4, Banjo 5, Drumstick 6, Pipsy 7, T.T. 8, Diddy 9. |
| `DKR_AUTORUN_VEHICLES=all` | Also each other vehicle a track allows. |
| `DKR_AUTORUN_TIMEOUT=300` | Seconds before an unfinished race counts as a timeout. |
| `DKR_AUTORUN_LOOP=1` | Start over after the last race. |
| `DKR_AUTORUN_RESTART=1` | Start from the first race instead of resuming. |
| `DKR_AUTORUN_SNAPSHOT=1` | Save a snapshot (GPU and reference images) after each race, for `diff_frames.py`. |

### Anti-aliasing benchmark

Tap Test run, then AA bench. It is a short test run (about half an hour) that measures what anti-aliasing costs on
the heaviest races: Whale Bay, Jungle Falls and Frosty Village, the slowest tracks of hardware test run #10, raced
by Bumper four times each with anti-aliasing off, on, on, off (so neither setting gets the cooler console or the warm
caches). The game switches anti-aliasing itself before each race and puts your own setting back at the end. The
results issue ("[3DS test run] AA benchmark ...") has a table of the two settings side by side for each track and
what AA costs in GPU time and frame rate; the races are in `aa_bench_results.csv` (the test run's columns plus
`aa`), which `autorun_report.py` also splits by track and AA. It keeps its own progress file, so a full test run it
interrupts carries on afterwards. `DKR_AUTORUN_BENCH=1` runs it at every start instead; `DKR_AUTORUN_BENCH_TRACKS`
and `DKR_AUTORUN_BENCH_CHARACTER` change the tracks and the character.

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

`DKR_LOCKSTEP=1` (meant for the host build) makes runs repeatable: the N64 threads take turns like on the console's
single CPU, each retrace waits until every thread is done with the previous one, and the game's clock advances by
exactly 1/60 s per retrace, so the same build always draws the same frames, as fast as the machine allows.

`port/tools/frame_regression.py` uses it to compare two host builds:

```
python3 port/tools/frame_regression.py --rom baserom.z64 --head port/build/host/dkr --base old/dkr --out /tmp/regression
```

It plays a few scripted scenarios (the intro and title, a spread of levels through `DKR_WARP`, and the Adventure hub
from the complete save) on both builds, and writes `report.md` and side by side images (base, head, difference) of
every frame that differs.

### CI

`.github/workflows/Port.yml` runs on every push: it builds `dkr.3dsx` and the host build (both are uploaded as
workflow artifacts), then runs the frame regression between this commit and the previous one, with its report on
the run's summary page. It needs the `ROMURL` repository secret, a private download URL for the US 1.0 ROM (the
same secret the N64 workflows use).

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
  spare (see the profiler above).
- **Untested:** real hardware. Azahar does not model CPU timing exactly, so expect the real frame rate to be lower,
  especially on an Old 3DS.
- **Known gaps:**
  - Only the US 1.0 ROM is tested.
  - Rumble isn't supported.
