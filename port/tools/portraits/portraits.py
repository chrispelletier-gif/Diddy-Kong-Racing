"""Turns portrait-camera frames into 40x40 HUD portraits framed like the ROM's own (portrait_NAME_icon.png).
Run from the folder cap.sh wrote to: portraits.py OUT_DIR. SUBJECTS names the frame and the head's box in it."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from key import key
import cv2
import numpy as np
from PIL import Image, ImageFilter
H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../assets/.vanilla/us.v77/textures/2d/hud/')
SUBJECTS = {
    'tricky': ('boss38/frame_01400.png', (60, 92, 190, 222)),
    'bluey': ('bqhi/frame_01450.png', (110, 88, 212, 190)),
    'bubbler': ('bubbler3/frame_01250.png', (78, 32, 262, 216)),
    'smokey': ('sm330/frame_01450.png', (93, 8, 225, 140)),
    'wizpig': ('wizpig2/frame_01400.png', (95, 45, 225, 175)),
    'taj': ('tajC/frame_01450.png', (100, 22, 236, 158)),
}


def largest(k):
    """Keep only the biggest connected blob (drops rain, sparkles)."""
    a = k.getchannel('A').point(lambda v: 255 if v > 0 else 0)
    w, h = a.size
    px = a.load()
    label = [[0] * w for _ in range(h)]
    best, bestn, n = 0, 0, 0
    sizes = {}
    for y in range(h):
        for x in range(w):
            if px[x, y] and not label[y][x]:
                n += 1
                st = [(x, y)]; label[y][x] = n; c = 0
                while st:
                    cx, cy = st.pop(); c += 1
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < w and 0 <= ny < h and px[nx, ny] and not label[ny][nx]:
                            label[ny][nx] = n; st.append((nx, ny))
                sizes[n] = c
    best = max(sizes, key=sizes.get)
    out = k.copy()
    o = out.load()
    for y in range(h):
        for x in range(w):
            if label[y][x] != best:
                o[x, y] = (0, 0, 0, 0)
    return out


def retouch(crop):
    """Toward the ROM portraits' look: smooth painted shading instead of the game's blotchy textures, light from the
    top left, crisp edges and a dark outline."""
    big = crop.resize((144, 144), Image.BICUBIC)
    a = big.getchannel('A').point(lambda v: 255 if v > 100 else 0)
    # Close small gaps between parts (Wizpig's ear) so the backdrop doesn't show through them; they read as shadow.
    closed = a.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.MinFilter(7))
    rgb = np.asarray(big.convert('RGB')).copy()
    gap = (np.asarray(closed) > 0) & (np.asarray(a) == 0)
    rgb[gap] = (40, 16, 24)
    # Edge-keeping smoothing irons out texture noise but keeps eyes, horns and mouths sharp.
    for _ in range(3):
        rgb = cv2.bilateralFilter(rgb, 9, 30, 7)
    img = rgb.astype(np.float32) / 255
    # Soft volume shading: surface normals guessed from the blurred silhouette, lit from the top left.
    m = cv2.GaussianBlur(np.asarray(closed).astype(np.float32) / 255, (0, 0), 14)
    gy, gx = np.gradient(m)
    n = np.sqrt(gx * gx + gy * gy) + 1e-6
    light = (-(gx * -0.7 + gy * -0.7) / n) * np.clip(n * 60, 0, 1)
    img = img * (1 + 0.28 * light[..., None])
    hsv = cv2.cvtColor(np.clip(img, 0, 1), cv2.COLOR_RGB2HSV)
    hsv[..., 1] = np.clip(hsv[..., 1] * 1.25, 0, 1)
    img = cv2.cvtColor(hsv, cv2.COLOR_HSV2RGB)
    # Lift dark models (Tricky) so their brightest lit areas come near white, as the ROM portraits do.
    top = np.percentile(img.max(axis=2)[np.asarray(closed) > 0], 97)
    img = (img / max(top, 0.5)) ** 0.85
    rgb = Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))
    edges = rgb.convert('L').filter(ImageFilter.FIND_EDGES).point(lambda v: 0 if v < 50 else min(120, v))
    rgb = Image.composite(Image.new('RGB', rgb.size, (30, 12, 20)), rgb, edges)
    rgb = rgb.filter(ImageFilter.UnsharpMask(radius=2, percent=70, threshold=3))
    out = rgb.convert('RGBA')
    out.putalpha(closed)
    rim = Image.new('RGBA', out.size, (20, 10, 40, 0))
    rim.putalpha(closed.filter(ImageFilter.MaxFilter(7)))
    rim.alpha_composite(out)
    return rim


def frame_like_original(subject):
    base = Image.open(H + 'diddy_icon.png').convert('RGBA')
    out = base.copy()
    px = out.load()
    for y in range(2, 38):
        for x in range(2, 38):
            px[x, y] = (0, 92 - (y - 2) * 18 // 35, 255, 255)
    inner = Image.new('RGBA', (36, 36), (0, 0, 0, 0))
    s = subject.resize((36, 36), Image.LANCZOS)
    inner.alpha_composite(s)
    out.alpha_composite(inner, (2, 2))
    # The original's frame edges, put back over the subject.
    for y in range(40):
        for x in range(40):
            if x < 2 or y < 2 or x >= 38 or y >= 38:
                px[x, y] = base.getpixel((x, y))
    return out


if __name__ == '__main__':
    out = sys.argv[1]
    for name, (path, box) in SUBJECTS.items():
        k = largest(key(path))
        crop = k.crop(box)
        frame_like_original(crop).save(out + '/' + name + '_plain.png')
        frame_like_original(retouch(crop)).save(out + '/' + name + '_icon.png')
    print('ok')
