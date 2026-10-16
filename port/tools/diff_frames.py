#!/usr/bin/env python3
"""
Compares the frame pairs written by the 3DS build's differential test mode (DKR_DIFF_EVERY).

For every frame N there is diff_N_ref.png (the software reference renderer, 320x240) and diff_N_gpu.bin (the 3DS
GPU's render target: u32 width, u32 height, then RGBA8 pixels in the framebuffer's rotated layout). The GPU image
is turned upright, cropped to the 320 pixel game area and compared with the reference.

Usage: diff_frames.py <directory with the frames> <output directory> [--threshold PERCENT]

Writes report.txt (one line per frame, worst first) and, for frames whose share of clearly different pixels is above
the threshold, side by side images (reference | GPU | difference) to look at.
"""
import argparse
import glob
import os
import re
import struct
import sys

from PIL import Image, ImageChops

GAME_W, GAME_H = 320, 240


def load_gpu(path):
    with open(path, "rb") as f:
        w, h = struct.unpack("<II", f.read(8))
        data = f.read(w * h * 4)
    return w, h, data


def to_image(w, h, data, order):
    img = Image.frombytes("RGBA", (w, h), data, "raw", order).convert("RGB")
    if w > 240:
        # Anti-aliased capture at twice the resolution: average it down the way the display transfer does.
        img = img.resize((w // 2, h // 2), Image.Resampling.BOX)
    return img


def gpu_candidates(w, h, data):
    """Every plausible way to interpret the capture: byte order and orientation."""
    for order in ("ABGR", "RGBA"):
        img = to_image(w, h, data, order)
        for transpose in (Image.Transpose.ROTATE_90, Image.Transpose.ROTATE_270):
            upright = img.transpose(transpose)  # 400 x 240
            for flip in (None, Image.Transpose.FLIP_LEFT_RIGHT, Image.Transpose.FLIP_TOP_BOTTOM):
                candidate = upright.transpose(flip) if flip is not None else upright
                x0 = (candidate.width - GAME_W) // 2
                yield (order, transpose, flip), candidate.crop((x0, 0, x0 + GAME_W, GAME_H))


def apply_layout(w, h, data, layout):
    order, transpose, flip = layout
    img = to_image(w, h, data, order).transpose(transpose)
    if flip is not None:
        img = img.transpose(flip)
    x0 = (img.width - GAME_W) // 2
    return img.crop((x0, 0, x0 + GAME_W, GAME_H))


def compare(ref, gpu):
    diff = ImageChops.difference(ref, gpu).convert("L")
    hist = diff.histogram()
    total = GAME_W * GAME_H
    mean = sum(i * n for i, n in enumerate(hist)) / total
    bad = sum(hist[48:]) / total * 100.0  # Clearly different pixels, beyond filtering and rounding differences.
    return mean, bad, diff


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("frames")
    parser.add_argument("out")
    parser.add_argument("--threshold", type=float, default=8.0, help="percent of clearly different pixels to flag")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    pairs = []
    for ref_path in sorted(glob.glob(os.path.join(args.frames, "diff_*_ref.png"))):
        frame = int(re.search(r"diff_(\d+)_ref", ref_path).group(1))
        gpu_path = ref_path.replace("_ref.png", "_gpu.bin")
        if os.path.exists(gpu_path):
            pairs.append((frame, ref_path, gpu_path))
    if not pairs:
        sys.exit("no frame pairs found")

    # Work out the capture layout from the first pair whose reference has some detail.
    layout = None
    for frame, ref_path, gpu_path in pairs:
        ref = Image.open(ref_path).convert("RGB")
        if ref.getextrema()[0][1] < 32:
            continue
        w, h, data = load_gpu(gpu_path)
        layout = min(gpu_candidates(w, h, data), key=lambda c: compare(ref, c[1])[0])[0]
        break
    if layout is None:
        sys.exit("all frames are blank")

    results = []
    for frame, ref_path, gpu_path in pairs:
        ref = Image.open(ref_path).convert("RGB")
        w, h, data = load_gpu(gpu_path)
        gpu = apply_layout(w, h, data, layout)
        mean, bad, diff = compare(ref, gpu)
        results.append((bad, mean, frame))
        if bad >= args.threshold:
            sheet = Image.new("RGB", (GAME_W * 3, GAME_H))
            sheet.paste(ref, (0, 0))
            sheet.paste(gpu, (GAME_W, 0))
            sheet.paste(diff.point(lambda v: min(255, v * 4)).convert("RGB"), (GAME_W * 2, 0))
            sheet.save(os.path.join(args.out, "flagged_%05d.png" % frame))

    results.sort(reverse=True)
    with open(os.path.join(args.out, "report.txt"), "w") as f:
        f.write("layout %s\n" % (layout,))
        for bad, mean, frame in results:
            f.write("frame %5d  different %5.1f%%  mean %5.1f%s\n" % (frame, bad, mean,
                                                                     "  FLAGGED" if bad >= args.threshold else ""))
    flagged = sum(1 for r in results if r[0] >= args.threshold)
    print("%d frames compared, %d flagged (layout %s)" % (len(results), flagged, layout))


if __name__ == "__main__":
    main()
