# Comparing the renderer with a real N64

The port draws DKR by interpreting its display lists itself (`src/gfx/gfx_rdp.c`: the F3DDKR microcode's work on the
CPU, the RDP's colour combiner and blender translated for the GPU). These tools check that against an accurate N64:

1. **ares** runs the ROM with a low level RSP, so the game's own F3DDKR microcode transforms, clips and sets up every
   triangle, and paraLLEl-RDP (a bit exact RDP) draws them. `ares-headless` (`headless/`, with the small hooks in
   `ares-hooks.patch`) is a frontend without a window that plays scripted input and, for chosen graphics tasks, saves
   RDRAM as the task starts and the picture the RDP has drawn when it finishes.
2. **replay** (`replay.c`) loads one such task into the port's interpreter and software renderer (the host build's
   `gfx_rdp.o` and `gfx_soft.o`), byte swapping the display list's data on the way, so it draws exactly the same
   frame from exactly the same memory.
3. **compare.py** measures the difference and writes side by side images (N64, port, difference).

Because both sides draw the same display list, game timing, randomness and the port's own game code play no part:
any difference is the renderer's.

## Running it

On Ubuntu 24.04 (the cloud sessions' image), with the US 1.0 ROM in `baseroms/baserom.us.v77.z64` and the assets
extracted (see the port's README):

```
port/tools/n64_compare/n64_compare.sh setup                     # ares source from Ubuntu, patched and built
port/tools/n64_compare/n64_compare.sh capture attract /tmp/att  # intro, character intros, title over many tracks
port/tools/n64_compare/n64_compare.sh capture race /tmp/race    # menus and a Tracks race on Ancient Lake, with HUD
port/tools/n64_compare/n64_compare.sh replay /tmp/race /tmp/race_out
```

A capture takes about 25 minutes (the RDP runs on the CPU through lavapipe) and 1 MB per saved frame. `replay` prints
the frames that differ most and writes `report.csv` and `*_sbs.png`. After changing the renderer, rebuild the host
build and `make -C port/tools/n64_compare`, then replay the same capture again.

## Results (October 2026)

228 frames: the intro, the character intros, the title screen's demos over most tracks, the menus, and a Tracks race
with the HUD.

- Geometry, the F3DDKR commands (matrices, vertex loads, triangle lists, billboards, culling), texture formats and
  palettes, the colour combiner and the blender all match: models, text and effects land on the same pixels, with
  differences only along edges (the N64 anti-aliases them) and in the N64's dithering.
- Fixed: bilinear filtered textures were drawn half a texel off (the RDP puts texel centres on whole coordinates, GPUs
  half a texel further on). This was the largest difference overall; the HUD and text in particular are now closer.
  Average difference per colour channel: title and demos 5.24 to 4.42, race 5.79 to 4.65.
- Fixed: the screen clear left the rightmost column of the depth buffer as it was (in fill mode the RDP includes the
  scissor's right edge, and DKR clears with the scissor at 319), and copy mode rectangles were filtered.
- Remaining, by design: the N64's 16 bit colour and dithering, its edge anti-aliasing, and its 3-point texture
  filter (bilinear here; the difference measured is negligible).
- Remaining: on very large polygons close to the camera (some cave floors and dirt roads), vertex colours spread
  differently. The RDP interpolates them in screen space across the triangles the microcode clipped to twice the
  screen's size, while the GPU interpolates with perspective correction. Imitating the clipping and interpolation
  in the software renderer fixed the worst cave frame (35% of pixels off to 1%) but made a dirt road frame worse, so
  matching it exactly would mean reproducing the microcode's clipper.
