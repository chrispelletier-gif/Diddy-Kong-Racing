import sys
from PIL import Image, ImageDraw
from collections import deque
def key(path):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    px = im.load()
    alpha = Image.new('L', (w, h), 255)
    a = alpha.load()
    seen = bytearray(w * h)
    q = deque()
    for x in range(w):
        q.append((x, 0)); q.append((x, h - 1))
    for y in range(h):
        q.append((0, y)); q.append((w - 1, y))
    while q:
        x, y = q.popleft()
        if seen[y * w + x]:
            continue
        seen[y * w + x] = 1
        c = px[x, y]
        # background: close to any seed colour of the edge it came from; use local similarity to neighbours
        a[x, y] = 0
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx]:
                n = px[nx, ny]
                if sum(abs(n[i] - c[i]) for i in range(3)) < 24:
                    q.append((nx, ny))
    out = im.convert('RGBA')
    out.putalpha(alpha)
    return out
if __name__ == '__main__':
    for p in sys.argv[1:]:
        k = key(p)
        print(p, k.getbbox())
