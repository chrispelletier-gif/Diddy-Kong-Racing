"""Turns portrait-camera frames into 40x40 HUD portraits framed like the ROM's own (portrait_NAME_icon.png).
Run from the folder cap.sh wrote to: portraits.py OUT_DIR. SUBJECTS names the frame and the head's box in it."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from key import key
from PIL import Image, ImageEnhance, ImageFilter, ImageOps
H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../assets/.vanilla/us.v77/textures/2d/hud/')
SUBJECTS = {
    'tricky': ('boss38/frame_01400.png', (66, 98, 184, 216)),
    'bluey': ('blueyA/frame_01575.png', (86, 68, 181, 163)),
    'bubbler': ('bubbler3/frame_01250.png', (92, 60, 242, 210)),
    'smokey': ('smokeyC/frame_01475.png', (105, 8, 215, 118)),
    'wizpig': ('wizpig2/frame_01400.png', (95, 45, 225, 175)),
    'taj': ('tajB/frame_01575.png', (97, 56, 223, 182)),
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
    """Toward the ROM portraits' look: brighter fill light, richer colour, crisp edges and a dark outline."""
    big = crop.resize((144, 144), Image.LANCZOS)
    a = big.getchannel('A')
    rgb = big.convert('RGB').filter(ImageFilter.MedianFilter(3))
    rgb = ImageOps.autocontrast(rgb, cutoff=1, preserve_tone=True)
    rgb = ImageEnhance.Color(rgb).enhance(1.35)
    rgb = rgb.point(lambda v: int(255 * ((v / 255) ** 0.8)))
    # Ink where the shading changes sharply (horn edges, eyes), as drawn art has.
    edges = rgb.convert('L').filter(ImageFilter.FIND_EDGES).point(lambda v: 0 if v < 40 else min(160, v))
    ink = Image.new('RGB', rgb.size, (30, 12, 20))
    rgb = Image.composite(ink, rgb, edges)
    rgb = rgb.filter(ImageFilter.UnsharpMask(radius=2, percent=90, threshold=3))
    out = rgb.convert('RGBA')
    out.putalpha(a)
    rim = Image.new('RGBA', out.size, (20, 10, 40, 0))
    rim.putalpha(a.filter(ImageFilter.MaxFilter(9)).point(lambda v: 255 if v > 128 else 0))
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
