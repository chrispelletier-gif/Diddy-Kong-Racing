# Portraits from the game's 3D models

The ROM has 40x40 portraits for the ten racers only. These tools draw the others (the bosses and Taj) from their
in-game models, for the bottom screen.

1. Build the PC version (`make -C port TARGET=host`).
2. From a folder holding `baserom.z64`, capture each subject with `cap.sh name dist height yaw VARS...`. It uses
   `DKR_PORTRAIT` (port/src/game/port_portrait.c), which puts the camera in front of one model and leaves out the
   track, sky, fog, shadows, HUD and every other object. The settings used for the 2026-10-26 set:

   | Subject | Command |
   |---|---|
   | Tricky  | `cap.sh boss38 180 70 90 DKR_PORTRAIT=boss DKR_AUTORUN=1 DKR_AUTORUN_BOSS=1 DKR_AUTORUN_BOSS_TRACKS=38` |
   | Bluey   | `cap.sh bqhi 170 140 165 ... DKR_AUTORUN_BOSS_TRACKS=1 DKR_PORTRAIT_PITCH=25 DKR_DUMP_EVERY=25` (frame 1450, front from above) |
   | Bubbler | `cap.sh bubbler3 220 50 270 ... DKR_AUTORUN_BOSS_TRACKS=40` |
   | Smokey  | `cap.sh sm330 150 65 330 ... DKR_AUTORUN_BOSS_TRACKS=41 DKR_PORTRAIT_PITCH=8 DKR_DUMP_EVERY=25` (frame 1450) |
   | Wizpig  | `cap.sh wizpig2 130 105 90 ... DKR_AUTORUN_BOSS_TRACKS=37` (frame 1400) |
   | Taj     | `cap.sh tajC 110 45 225 DKR_PORTRAIT=taj DKR_WARP=0 DKR_WARP_SECONDS=60 DKR_DUMP_EVERY=25` (frame 1450, head to knees) |

3. `portraits.py OUT` keys out the background (key.py), keeps the largest shape, crops each head (boxes in
   `SUBJECTS`) and frames it like the ROM's portraits.

4. Optionally repaint each one with an image model (the 2026-10-26 set used SpriteCook: the capture as the image to
   edit, a sheet of four ROM portraits as the style guide), then crop and frame it at 40x40 the same way.
5. Copy them to `sdmc:/3ds/dkr/portraits/NAME.png` (tricky, bluey, bubbler, smokey, wizpig, taj). The game only takes
   40x40 PNGs; anything else falls back to a name box. Keep them out of the repo: they are Rare's characters.

The racers' in-game models are much plainer than the ROM's pre-rendered portraits, so those stay as they are.

Needs Pillow, numpy and opencv-python-headless.
