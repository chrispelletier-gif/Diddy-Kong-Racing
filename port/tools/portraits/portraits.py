"""Turns portrait-camera frames into 40x40 HUD portraits framed like the ROM's own (portrait_NAME_icon.png).
Run from the folder cap.sh wrote to: portraits.py OUT_DIR. SUBJECTS names the frame and the head's box in it."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from key import key
from PIL import Image, ImageFilter
H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../assets/.vanilla/us.v77/textures/2d/hud/')
SUBJECTS = {
    'tricky': ('boss38/frame_01400.png', (80, 114, 170, 204)),
    'bluey': ('bluey7/frame_01350.png', (78, 114, 163, 199)),
    'bubbler': ('bubbler3/frame_01250.png', (110, 96, 220, 206)),
    'smokey': ('smokey3/frame_01250.png', (140, 38, 250, 148)),
    'wizpig': ('wizpig/frame_01400.png', (105, 52, 225, 172)),
    'taj': ('taj4/frame_01250.png', (126, 90, 200, 164)),
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
        frame_like_original(crop).save(out + '/' + name + '_icon.png')
        crop.resize((crop.width * 2, crop.height * 2), Image.LANCZOS).save(out + '/' + name + '_large.png')
    print('ok')
