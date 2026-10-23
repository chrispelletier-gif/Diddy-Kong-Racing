#!/usr/bin/env python3
"""Compares replayed frames with the N64's.

    compare.py <dir> [top]

Reads <dir>/*_n64.png and *_port.png (from run_replays.sh), writes <dir>/report.csv and a side by side image per frame
(<dir>/*_sbs.png: N64, port, difference), and prints the frames that differ most.

Two numbers per frame:
  mean_abs_diff   average difference per colour channel (0-255), after a 3x3 blur that hides the N64's dithering
  pct_pixels_off  share of pixels more than 40 apart, comparing each pixel with its best match within one pixel in the
                  other image (so anti-aliased edges and sub-pixel shifts don't count)
"""
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

W, H = 320, 240


def nearest_diff(x, y):
    best = None
    padded = np.pad(y, ((1, 1), (1, 1), (0, 0)), mode='edge')
    for dy in range(3):
        for dx in range(3):
            d = np.abs(x - padded[dy:dy + H, dx:dx + W]).max(axis=2)
            best = d if best is None else np.minimum(best, d)
    return best


def main():
    d = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 15
    rows = []
    for f in sorted(glob.glob(os.path.join(d, '*_n64.png'))):
        name = os.path.basename(f)[:-len('_n64.png')]
        p = os.path.join(d, name + '_port.png')
        if not os.path.exists(p):
            continue
        a = Image.open(f).convert('RGB')
        b = Image.open(p).convert('RGB')
        ab = np.asarray(a.filter(ImageFilter.BoxBlur(1)), dtype=np.int16)
        bb = np.asarray(b.filter(ImageFilter.BoxBlur(1)), dtype=np.int16)
        diff = np.maximum(nearest_diff(ab, bb), nearest_diff(bb, ab))
        rows.append((name, float(np.abs(ab - bb).mean()), float((diff > 40).mean() * 100)))
        heat = np.zeros((H, W, 3), np.uint8)
        heat[..., 0] = np.clip(diff * 4, 0, 255)
        sbs = Image.new('RGB', (W * 3, H))
        sbs.paste(a, (0, 0))
        sbs.paste(b, (W, 0))
        sbs.paste(Image.fromarray(heat), (W * 2, 0))
        sbs.save(os.path.join(d, name + '_sbs.png'))
    with open(os.path.join(d, 'report.csv'), 'w') as fp:
        fp.write('frame,mean_abs_diff,pct_pixels_off\n')
        for r in rows:
            fp.write('%s,%.2f,%.2f\n' % r)
    if rows:
        print('%d frames: mean_abs_diff %.2f, pct_pixels_off %.2f' %
              (len(rows), sum(r[1] for r in rows) / len(rows), sum(r[2] for r in rows) / len(rows)))
    for r in sorted(rows, key=lambda r: -r[2])[:top]:
        print('  %s  mean %.2f  off %.2f%%' % r)


if __name__ == '__main__':
    main()
