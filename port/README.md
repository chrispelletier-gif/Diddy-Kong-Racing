# Diddy Kong Racing for Nintendo 3DS

A native port of the Diddy Kong Racing decompilation to the Nintendo 3DS. The game code is the decompiled C from
this repository, compiled for ARM. The N64 hardware it expects is provided by a small emulation layer:

- the libultra OS calls, mapped onto threads
- ROM DMA and saves
- an interpreter for the F3DDKR display lists, drawn with the 3DS GPU
- a C version of the RSP audio microcode

Nothing from the game is distributed. The port reads every asset from your own ROM at runtime.

This file is for developers: building, the debug build's tools, how the port works and how it is tested. Players
should read the [main README](../README.md) instead, which covers installing and playing a release.

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

The result is `port/build/3ds/dkr.3dsx`, a debug build: its Settings menu has the profiler, the test runs, bug
reports, updates and the performance switches (16-bit buffers, render ahead, edge trimming, the sound engine's
switches). `make -C port TARGET=3ds RELEASE=1` builds the one for players, `port/build/3ds-release/dkr.3dsx`, whose
menus offer only the settings a player needs; the hidden switches keep their defaults, SELECT + touch does nothing and
the tools' code is still in (`PLAT_DEBUG_TOOLS` in `port/src/plat/plat.h`). CI builds both; the updater installs debug
builds.

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
| L             | Z                   | Use item                |
| ZL            | (port)              | Look behind             |
| R or ZR       | R                   | Hop / drift             |
| X             | C-Up                |                         |
| Y             | C-Left              |                         |
| C-Stick       | C buttons           | (New 3DS)               |
| D-Pad         | D-Pad               |                         |
| SELECT        | L                   |                         |
| START         | START               | Pause                   |

That is the default layout. A, B, X, Y, L, R, ZL, ZR and SELECT can be remapped (with presets for A/B swapped and L/R
swapped), and the circle pad's deadzone (default 6%), sensitivity and response curve changed, on the settings menu's
Controls page. They are kept in `sdmc:/3ds/dkr/controls.txt`. START, the D-Pad and the C-Stick always stay as above.

The menus are driven by the stick, as on the N64. At the bottom of the bottom screen is a Settings button (and Stop
test while a test run is on; and Pause, first, during a race). The rest of the touch screen does nothing while the menu
is closed, so a stray tap can't interrupt a race; holding SELECT while touching anywhere also sends a bug report.

## Race HUD on the bottom screen (3DS)

During a one player race (not time trials or the challenges, which keep the N64's HUD) the bottom screen shows the
race (`src/plat/bottom_screen.c`, from `src/game/port_race_hud.c`): every racer in race order as the game's portrait,
framed in their map colour, with the time gap to you (red: ahead of you, green: behind, timed at the track's
checkpoints); the item you hold at twice size; your place, lap and race time; your bananas with your last and best lap
times; and the track's map with every racer on it, you as a gold arrow. It is all the game's own HUD art at its own
size or exactly twice, read from your ROM. The top screen then keeps only the countdown, the lap announcements, the
direction arrows, WRONG WAY and the finish. The Race HUD page of the Settings menu (and VIDEO OPTIONS) moves it:
Bottom (the default), Both screens, or Top (the N64's HUD only); saved as `racehud=` (1, 2, 0). The profiler's
readout takes the bottom screen's place while it is on.

The ROM has no portraits for the bosses and Taj, and drawn ones of Rare's characters cannot be shipped here, so they
come from your SD card: `sdmc:/3ds/dkr/portraits/` with `tricky.png`, `bluey.png`, `bubbler.png`, `smokey.png`,
`wizpig.png` and `taj.png`, each a 40x40 PNG framed like the game's portraits (`port/tools/portraits` makes them from
your ROM). A file that is missing, is not a PNG or is any other size is skipped (the log says so) and that racer gets
a box with its initial instead.

The game is one player only: a 3DS has one set of controls, so nobody else can join character select, and the magic
codes that only matter with more players (DOUBLEVISION, JOINTVENTURE) are answered like wrong codes and left out of the
code list. The 2 to 4 player code is all still there for local wireless play later (roadmap O9): building with
`make -C port TARGET=3ds PORT_MULTIPLAYER=1` brings it back. Test runs and CI never play multiplayer.

## Settings in the game's menus

The same settings are also in the game's own menus, drawn in its fonts and dialogue boxes (`src/menu.c`, with the
categories in `src/plat/options.c`). The Options menu gets VIDEO OPTIONS (anti-aliasing, frame rate, 16-bit buffers,
mipmaps, colour boost, widescreen, 800 pixels, stereo 3D), CAMERA OPTIONS and CONTROL OPTIONS rows next to the game's
own, and the Audio Options screen a MORE AUDIO OPTIONS row with the Sound settings. The pause menu gets an OPTIONS row
that lists the same categories, without the settings that only apply after a restart. Up and down pick a row, left
and right (or A) change it, B goes back. Both menus change the same saved values as the bottom screen's Settings menu,
which redraws when the game's menus change something it shows.

## Settings menu (3DS)

Settings opens a menu over the bottom screen; tap a row to change it (the left half of a value steps back), and Close,
Back, Prev and Next are where the button was. Choices are saved in `sdmc:/3ds/dkr/settings.txt`.

The bottom screen is drawn in the game's own style (`src/plat/bottom_screen.c`): its title logo, menu title font, small
font, colourful font, blue arrows and ON / OFF labels, all read from your ROM like the game reads them, so nothing of
the game is shipped. Taps on the menu play the game's own menu sounds. Until the ROM is loaded the screen uses a plain
8x8 font.
- **Anti-aliasing**: on or off, from the next frame (see Graphics).
- **Mipmaps**: Off, On, or Show (on, with the levels tinted red, green, blue and yellow, to see where the GPU uses
  them), from the next frame (see Graphics).
- **Colour boost**: Off, Low, Medium or High, from the next frame (see Graphics).
- **Render ahead**: 1 frame (the N64's timing: the game builds a frame while the renderer draws the one before, and
  waits for it) or 2 frames (the game runs one frame further ahead, so a slow frame in the renderer is absorbed
  instead of dropped, at the cost of one frame more input lag; the game keeps a third display list heap for it).
  `DKR_RENDER_AHEAD=2` in `env.txt` tries it without the menu; it also works on the host build.
- **Profiler**: frame times on the bottom screen (see Profiler).
- **Test run**: start a full test, a quick test, the boss fights or the GPU bench, or stop the one running.
- **Controls**: button layout, remapping and circle pad deadzone, sensitivity and response (see Controls).
- **Sound** (roadmap Phase 7):
  - *Sound engine*: DSP voices (default) plays every voice of the game's music and sound effects on one of the 3DS
    DSP's 24 channels, which decode, resample, envelope and mix it; the CPU only runs the game's sequencer and the
    N64 reverbs (see Sound below). CPU mixer runs the N64 audio microcode on the CPU instead, as before. Applies on
    the next start. The mixing core, rate and buffer rows only apply to the CPU mixer; with the DSP voices they
    read "CPU mixer only".
  - *Mixing core*: Renderer (default) mixes the sound next to the renderer. System mixes it on the 3DS's system
    core instead, with the application's share of it raised to 80% and below the system's own services there
    (the first try, at the renderer's priority, held up the GPU's service and doubled frame times). Applies on the
    next start.
  - *Mixer rate*: 22 kHz (the game's own) or 32 kHz, which keeps more of the high frequencies when sounds are
    pitched up, for about 45% more mixing work. Pitches and reverb lengths are scaled to sound the same. Applies on
    the next start.
  - *Smooth resampling*: ON (default) lets the DSP use its polyphase filter to resample (each voice with the DSP
    voices, the whole mix with the CPU mixer) instead of linear interpolation, which is softer on high frequencies.
    Free on the CPU; from the next sound.
  - *Sound buffer*: Large keeps 50 ms more sound queued, so a late audio frame doesn't leave a gap, at the cost of
    the sound coming 50 ms later.
- **Check for updates** and **Send a bug report** (see below).

Other parts of the port add a page to the menu with `menu_3ds_add_page` (`3ds/menu_3ds.h`): its rows are a label, a
value and a change function, and the main page gets a row that opens it.

## How it works

| Path                       | What it does |
| -------------------------- | ------------ |
| `src/os/`                  | libultra replacement: threads, message queues, events, timers, PI (ROM), SI (controllers, EEPROM, Controller Pak), VI, AI and the RCP. Every N64 thread is a real thread; on the 3DS they all share one core with strict priorities, as on the N64. |
| `src/game/asset_swap.c`    | The ROM is big endian, so each asset is converted to native byte order as it is loaded. |
| `src/game/`                | Other per-port game support: C versions of the few hand-written assembly routines, and zlib inflate. |
| `src/gfx/gfx_rdp.c`        | F3DDKR display list interpreter. Transforms vertices on the CPU, decodes and caches textures, decodes the RDP colour combiner and blender. |
| `3ds/gfx_c3d.c`            | citro3d backend. Compiles the combiner to TEV stages. |
| `host/gfx_soft.c`          | Software rasteriser backend for the host build. |
| `src/audio/audio_rsp.c`    | C implementation of the aspMain audio microcode (the CPU mixer). The resampler table is read from the ROM. |
| `src/audio/voices.c`       | The DSP voices: the synthesizer's voice changes as DSP channel settings, the samples decoded, the N64 reverbs on the aux buses (see Sound). |
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
| `DKR_DUMP_AUDIO`    | Write the audio output to this file as raw 16 bit stereo PCM (22050 Hz with the CPU mixer, 32728 Hz with the DSP voices). |
| `DKR_AUDIO_DSP`     | `0` runs the CPU mixer instead of the DSP voices (see Sound). |
| `DKR_TRACE_FRAME`   | Log every display list command and draw of this frame. |
| `DKR_BOTTOM_PREVIEW`| Draw the 3DS bottom screen's views (home, profiler, menu pages, updater) to PNG files in this directory and exit. `DKR_MONO_FONT` can name a copy of libctru's 8x8 font (2048 bytes) for the dense text. |
| `DKR_BOTTOM_RACE`   | Draw the 3DS bottom screen's race HUD (roadmap G9) to `race_NNNN.png` in this directory every 60 frames of a race, and leave those HUD pieces off the top screen as the 3DS does (`DKR_RACE_HUD=2` keeps them on both). `DKR_BOTTOM_RACE_LOG=1` also prints the player's map position and heading each frame. |

`make -C port TARGET=host DEBUG=asan BUILD_DIR=build/asan` builds with AddressSanitizer, which is how the original
memory bugs above were found.

## Graphics

The top screen is anti-aliased by default: frames are drawn at twice the resolution in each direction and
downsampled when they are sent to the screen. It only costs GPU time (the CPU work is unchanged). Turn it off in the
Settings menu (until it has been set there, `DKR_AA=0` in `sdmc:/3ds/dkr/env.txt` also turns it off).

Mipmaps (on by default) stop distant track textures from shimmering: every texture that is opaque or repeats both
ways (ground, walls, cliffs, water) also gets smaller, pre-filtered copies when it is loaded, and the GPU reads the one that
fits the distance. That takes a third more memory for those textures. Clamped textures with transparency (shadows,
start lines, track borders) and point sampled ones (text) are left as they were.

Colour boost (off by default) makes colours more saturated, as the 3DS screen looks washed out next to a CRT. It is
done in the GPU's colour combiner for every draw, so it costs no time; greys, black and white stay as they were.
High is c + 0.5 (c - grey), Medium 0.3 and Low 0.15.

The host build can preview both: `DKR_MIPMAP=1` and `DKR_COLOUR=1` to `3` (both off by default there, so frames stay
comparable between commits).

The Settings menu's Screen page has the top screen's modes, all on by default:
- Widescreen fills the 400 pixel wide screen instead of a 4:3 picture in the middle. The game's camera sees more at
  the sides; the HUD, text and menus keep their 4:3 shape and place in the middle, except what reaches the edge of the
  N64 screen (fades, backgrounds, full screen fills), which is stretched to the edge.
- 800 pixels shows the screen's 800x240 mode while 3D is off (not on an original 2DS). The picture is drawn 800
  pixels wide, so it costs the GPU what anti-aliasing does; with anti-aliasing on it is the same 800x480 picture as
  before, only halved in height, so the extra detail across reaches the screen. Azahar doesn't show this mode (it
  shows the left half of the picture), so turn it off there.
- Stereo 3D follows the 3D slider. Each frame's GPU commands run twice, once for each eye with the picture moved
  across by an amount that grows with distance (the vertex shader's one addition), and the HUD sits at the depth of
  the screen. Anti-aliasing and the 800 pixel mode are off while the slider is up. `DKR_STEREO_SEP` (how far apart
  the eyes' pictures are for the farthest things, in thousandths of the half screen width, default 25) and
  `DKR_STEREO_CONV` (the distance in front of the camera that stays at the screen's depth, default 130, about where
  the kart is) in `env.txt` tune it.

The host build previews widescreen with `DKR_WIDESCREEN=1` (frames are then 400 pixels wide).

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
- `DKR_STATE_CACHE=0` (sends every GPU setting for every batch again, instead of only the ones that changed)
- `DKR_STATE_CACHE=2` (only the settings that changed, even when the texture changes; this hung the GPU on hardware)
- `DKR_VERTEX_FORMAT=48` (writes every vertex as 48 bytes of floats again, instead of 28 bytes with byte colours and
  the second texture's coordinates only when a draw uses one)
- `DKR_INDEXED=0` (writes a vertex for every triangle corner again, instead of one for each vertex that the
  triangles of a command share, drawn by index; about half the corners in the game's models are such repeats)
- `DKR_TEX_NATIVE=0` (stores every texture on the GPU as 32 bit RGBA8 again, instead of the 16 and 8 bit formats that hold it exactly)
- `DKR_MIPMAP=0` (never builds mipmaps, so they take no memory; the Settings switch then does nothing)
- `DKR_MIP_FILTER=0` (reads the nearest mipmap instead of blending the two nearest)
- `DKR_SINGLE_CORE=1` (on a New 3DS, keeps the renderer on the main core as on an Old 3DS, and the sound with it)
- `DKR_AUDIO_DSP=0` (the CPU mixer instead of the DSP voices, whatever the Sound page says; also on the host build)
- `DKR_AUDIO_DEBUG=1` (with the DSP voices, logs how many voices play and how late their changes were applied)
- `DKR_AUDIO_NOFX=1` (with the DSP voices, no reverb, for comparison)
- `DKR_DUMP_AUDIO=name` (with the DSP voices, writes the DSP's finished stereo output, 16 bit at its 32728 Hz, to
  `sdmc:/3ds/dkr/name`)
- `DKR_AUDIO_SIMD=0` (mixes the sound with the plain C loops instead of the ARM SIMD ones; the sound is the same)
- `DKR_AUDIO_RATE=n` (runs the mixer at n Hz, whatever the Sound page says; also works on the host build)
- `DKR_SYSCORE_LIMIT=n` (the share of the system core, in percent, asked for when the sound is mixed there; 80)
- `DKR_SYSCORE_PRIO=n` (the sound thread's priority on the system core, 0x18 highest to 0x3F lowest; 0x30)

## Bug reports

The 3DS build can send a bug report over Wi-Fi as an issue on this repository, labelled `3ds-bug-report`:
- Tap Settings, then Send a bug report, or hold SELECT and touch the screen anywhere. The game saves a
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
`last_crash.txt` after a crash) are posted as compressed comments on the issue. `log.txt` stops after the start-up
lines; the body has the latest ones. When the GPU stops responding, the game saves `gpu_hang.txt` (the GPU's command
queue, marking the command list it stopped in, and every draw of the stuck frame, those in that list starred) and
`gpu_hang_cmd.bin` (the frame's command list), and 20 seconds into any hang `hang_log.txt` (the latest log lines).
They go with a report sent right away, or as `last_*` files with the report on the next launch. To get them back:

```sh
python3 port/tools/bugreport_fetch.py --list        # open reports
python3 port/tools/bugreport_fetch.py 12             # saves issue 12 to bugreport_12/
python3 port/tools/diff_frames.py bugreport_12 bugreport_12/diff
```

A crash report gives `pc` and `lr`; with the `dkr.elf` of the same build (the `Build` line is its commit),
`arm-none-eabi-addr2line -f -e dkr.elf <pc> <lr>` names the functions.

## Updates

CI publishes every 3DS build as a pre-release: `3ds-newest` is the debug build of the latest push to any branch,
`3ds-master-debug` the debug build of master, and `3ds-master` the player build of master (the one to download; it has
no Check for updates button). Public releases are separate, fixed releases tagged by version (`3ds-v0.1...`),
with the player build's `.3dsx` and `.cia`; they don't change when master does. The updater only installs the two
debug builds. Tap Settings, then Check for updates; the game lists both builds (the
one running is marked "this") and offers master, newest or cancel. The chosen build is downloaded next to the
running `.3dsx`, checked against its size and sha256, and replaces it; tap Quit to finish the update, or keep
playing and the new build runs next time. It only works when the game was started from the SD card (not over
3dslink).

While the repository is public no token is needed. For a private repository the token in
`sdmc:/3ds/dkr/bugreport.txt` is used, and it needs Contents set to Read-only as well (step 4 above).

## Sound

By default the sound plays on the 3DS DSP as the system's own games do (roadmap Phase 7, "DSP voices"). The game's
sequence players and sound player run unchanged on the N64 synthesizer driver (libultra's `alAudioFrame`), which
allocates physical voices and queues their parameter changes (start, stop, volume, pan, pitch, effect mix) with
sample-accurate times. On the N64 the driver then builds a command list for the RSP, which the port runs on the CPU
(`src/audio/audio_rsp.c`). With the DSP voices no command list is built: each of the 24 physical voices is one DSP
channel (`src/audio/voices.c`), and its changes become channel settings, applied by the DSP's own frame clock (every
160 samples, 4.9 ms) so they keep their timing. The DSP decodes, resamples, envelopes and mixes every voice and
outputs at its own rate (about 32.7 kHz), so nothing is downsampled to 22 kHz on the way. The samples are decoded
from the ROM's VADPCM to 16 bit PCM once at start-up (12 MB of linear memory, about a second).
The HOME menu and sleep unload the DSP; when it is back the engine restarts its looping voices from their loops
and drops the one-shot sounds that were playing, since the firmware does not reliably carry the channels over.

The reverbs are the N64's own: the game's two effect buses (its custom one and the "big room") are the DSP's two
aux buses, and the voices' effect sends come back to the CPU every frame, where the same delay networks the RSP ran
(`alFxPull`) are applied as plain arithmetic (`pv_aux_process`) before the DSP mixes the returns in. libctru does not
expose that aux path, so `3ds/audio_ndsp.c` reads and writes the DSP's shared memory region for it directly.

What differs from the N64: the DSP's resampler, envelope steps every 4.9 ms instead of every 8 samples, 24 physical
voices instead of 40 (the driver steals the quietest, as it did when the N64 ran out), and the reverb's chorus taps
use linear interpolation. The CPU mixer stays as a Settings > Sound fallback. The host build has a software
stand-in for the channels (`host/audio_soft.c`), so `DKR_DUMP_AUDIO` records the same engine on a PC.

## Profiler

Turn Profiler on in the Settings menu to show the profiler on the bottom screen, updated every second (it appears
when the menu closes); the game remembers the setting. `DKR_PROFILE=1` in `env.txt` and the test run
below turn it on as well. While it is off it costs nothing measurable (each timing hook returns after one check).

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
line also goes to the log, followed on the 3DS by a `renderer parts` line that splits `rdp` into the backend's draw
calls (what is left is the display list walk) and, within them, the guard band clipping, the GPU settings and the
vertex writes, in ms per frame.

## Unattended test run

Tap Settings, Test run, then Full test, and leave the console on its charger. From the
title screen, the game races every track with every character (200 races, some 8 hours), with the AI driving your
kart. Each race is profiled and checked: a race that doesn't finish within 5 minutes, a kart that stops reaching
new checkpoints for a minute, a crash or a freeze are all recorded. When your kart gets no further along the course
for 10 s (wedged, circling, or fallen under the track), it is put at the next checkpoint and the race goes on; the spot is noted in the results. The bottom screen shows the profiler, the race, and a
progress bar. Your saves are not written while it runs. Tap Stop test (next to Settings) to end it early.

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
| `DKR_AUTORUN=1` | Run at every start, without the menu. |
| `DKR_AUTORUN_TRACKS=5,3` | Only these tracks (`ASSET_LEVEL_*` ids; default: the 20 of the tracks menu). |
| `DKR_AUTORUN_CHARACTERS=3,9` | Only these characters: Krunch 0, Bumper 1, Tiptup 2, Conker 3, Timber 4, Banjo 5, Drumstick 6, Pipsy 7, T.T. 8, Diddy 9. |
| `DKR_AUTORUN_VEHICLES=all` | Also each other vehicle a track allows. |
| `DKR_AUTORUN_TIMEOUT=300` | Seconds before an unfinished race counts as a timeout. |
| `DKR_AUTORUN_LOOP=1` | Start over after the last race. |
| `DKR_AUTORUN_RESTART=1` | Start from the first race instead of resuming. |
| `DKR_AUTORUN_SNAPSHOT=1` | Save a snapshot (GPU and reference images) after each race, for `diff_frames.py`. |

### Quick test

Tap Settings, Test run, then Quick test. It races only the slowest tracks of the full run (Haunted Woods, Jungle Falls, Crescent
Island, Whale Bay and EverFrost Peak, from hardware test run #12), twice each with Bumper and your own anti-aliasing
setting: about 20 minutes. Use it to tell whether a new build made the slow spots faster: run it on the old build,
install the new one, run it again. The results issue ("[3DS test run] quick test ...") compares each track with the
quick test before it on the same console, with both builds named, and gives the change in frame rate, share of
frames at 30 fps, GPU and renderer time and worst frame. The races are in `quick_test_results.csv` (the test run's
columns plus `build`), which `autorun_report.py` also splits by track and build. `DKR_AUTORUN_QUICK=1` runs it at
every start; `DKR_AUTORUN_QUICK_TRACKS`, `DKR_AUTORUN_QUICK_CHARACTER` and `DKR_AUTORUN_QUICK_PASSES` change the
tracks, the character and the races per track.

### Boss fights

Tap Settings, Test run, then Boss fights. Boss levels aren't on the tracks menu, so the full run never races them;
this races each of the ten boss fights once (Tricky, Bluey, Bubbler, Smokey and Wizpig, first and second fight) with
Bumper and the AI driving: about half an hour. The results issue ("[3DS test run] boss fights ...") has one line per
fight with its outcome, time, place, frame rate and how often the kart had to be put back on the course; a fall
through the ground shows up as unsticks or as a race that never finishes. The races are in `boss_test_results.csv`
(the test run's columns plus `build`). `DKR_AUTORUN_BOSS=1` runs it at every start; `DKR_AUTORUN_BOSS_TRACKS`,
`DKR_AUTORUN_BOSS_CHARACTER` and `DKR_AUTORUN_BOSS_PASSES` change the levels, the character and the races per boss.

### GPU benchmark

Tap Settings, Test run, then GPU bench. It is a short test run (about 50 minutes) that measures the render settings on
the heaviest races: Jungle Falls, Crescent Island and Haunted Woods, the slowest tracks of the quick tests, raced by
Bumper once with each of 8 settings: anti-aliasing 4x, 2x or off, 32 or 16-bit buffers, and 60 fps with AA off or 2x.
The game switches the settings itself before each race and puts your own back at the end. The results issue
("[3DS test run] GPU benchmark ...") has one line per track and setting, with the GPU time it saves against AA 4x and
how many frames made 60 fps; the races are in `gpu_bench_results.csv` (the test run's columns plus `config` and
`pct_60fps`), which `autorun_report.py` also splits by track and setting. It keeps its own progress file, so a full test
run it interrupts carries on afterwards. `DKR_AUTORUN_BENCH=1` runs it at every start instead;
`DKR_AUTORUN_BENCH_TRACKS`, `DKR_AUTORUN_BENCH_CHARACTER` and `DKR_AUTORUN_BENCH_CONFIGS` (indexes into the 8 settings)
change the tracks, the character and the settings. It replaced the anti-aliasing benchmark of roadmap item G0.

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

`port/tools/n64_compare/` checks the renderer against a real N64 instead: an accurate emulator (ares, running the
game's own F3DDKR microcode on its emulated RSP and paraLLEl-RDP for the RDP) plays the ROM and saves chosen frames'
display lists with the pictures it drew from them, and the port's renderer replays the same display lists, so both
sides draw exactly the same frame. See its README for how to run it and what it found.

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

- **Real hardware:** played on a New 3DS since 2026-10-17. Races hold 30 fps with 4x anti-aliasing, and about 60 fps
  with anti-aliasing off or 2x on most tracks (WizPig 1 and Walrus Cove 2 are GPU-bound at 60 fps with 2x).
- **Tested by machine:** every track with every character (the full test run), the ten boss fights, and the host
  build's frame regression on every push.
- **Not yet checked end to end:** a complete Adventure playthrough.
- **Known gaps:**
  - Only the US 1.0 ROM is supported; other versions are planned.
  - The Old 3DS hasn't been tried; the renderer counts on the New 3DS's extra core.
  - One player only (see above); rumble isn't possible on a 3DS.

Player defaults (the release build): anti-aliasing 4x, 30 fps, Balanced widescreen, 800 pixels and stereo 3D on,
HUD at the screen edges, race HUD on the bottom screen, mipmaps on, colour boost off, Modern camera with the wall
check, smooth resampling.
