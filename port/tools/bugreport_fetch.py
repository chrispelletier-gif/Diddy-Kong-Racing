#!/usr/bin/env python3
"""
Downloads a bug report sent from the 3DS build (SELECT + touch, or the offer after a crash) and turns its attachments
back into files.

The report is a GitHub issue labelled 3ds-bug-report. Its body is the readable summary; each file was gzip
compressed, base64 encoded and posted as one or more comments marked
    <!-- dkr-attachment name=NAME part=I parts=N size=BYTES encoding=gzip+base64 -->

Usage: bugreport_fetch.py <issue number or URL> [--out DIR] [--repo OWNER/NAME]
       bugreport_fetch.py --list [--repo OWNER/NAME]

Writes DIR/issue.md (the title and body) and every attached file. Set GITHUB_TOKEN for private repositories or a
higher rate limit. Snapshots (diff_N_ref.png + diff_N_gpu.bin) can then be compared with diff_frames.py. The frames
of the seconds before it that look most like a flash (diff_N_flash.bin) are also written as diff_N_flash.png: a row
for each, the frame before, it, and the frame after.
"""
import argparse
import base64
import gzip
import json
import os
import re
import struct
import sys
import urllib.request
import zlib

DEFAULT_REPO = "chrispelletier-gif/Diddy-Kong-Racing-3DS"
LABEL = "3ds-bug-report"
MARKER = re.compile(r"<!-- dkr-attachment name=(\S+) part=(\d+) parts=(\d+) size=(\d+) encoding=gzip\+base64 -->")


def get(api, path):
    request = urllib.request.Request(api + path, headers={"Accept": "application/vnd.github+json",
                                                          "User-Agent": "dkr-bugreport-fetch"})
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        request.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(request) as response:
        return json.load(response)


def get_all(api, path):
    items, page = [], 1
    while True:
        batch = get(api, f"{path}{'&' if '?' in path else '?'}per_page=100&page={page}")
        items += batch
        if len(batch) < 100:
            return items
        page += 1


def attachments(comments, author):
    """Returns {name: bytes} from the attachment comments, checking every part is there. Only the comments of the
    account that opened the issue (the console's) count, and a part's first copy wins: on a public repository anyone
    can comment, and must not be able to swap a report's files."""
    parts = {}
    for comment in comments:
        if (comment.get("user") or {}).get("login") != author:
            continue
        body = comment.get("body") or ""
        match = MARKER.search(body)
        if not match:
            continue
        name, part, count, size = match.group(1), int(match.group(2)), int(match.group(3)), int(match.group(4))
        fence = body.find("```", match.end())
        end = body.find("```", fence + 3)
        text = "".join(body[fence + 3:end].split())
        parts.setdefault(name, {"count": count, "size": size, "parts": {}})["parts"].setdefault(part, text)
    files = {}
    for name, info in parts.items():
        missing = [i for i in range(1, info["count"] + 1) if i not in info["parts"]]
        if missing:
            print(f"{name}: parts {missing} missing, skipped", file=sys.stderr)
            continue
        data = gzip.decompress(base64.b64decode("".join(info["parts"][i] for i in range(1, info["count"] + 1))))
        if len(data) != info["size"]:
            print(f"{name}: {len(data)} bytes, expected {info['size']}", file=sys.stderr)
        files[name] = data
    return files


def write_png(path, width, height, rgb):
    """Writes 8-bit RGB rows (bytes) as a PNG, without needing an image library."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def flash_to_png(data, path, scale=2):
    """diff_N_flash.bin (the letters DKRH, width, height, pictures, then each picture's frame number and RGB565
    pixels; three for each flash) as one PNG, a row of three for each flash, each picture scale times its size."""
    magic, w, h, count = struct.unpack_from("<4sIII", data)
    if magic != b"DKRH" or count == 0 or len(data) < 16 + count * (4 + w * h * 2):
        print(f"{path}: not a frame history file", file=sys.stderr)
        return
    rows, gap = (count + 2) // 3, 4
    width, height = 3 * w * scale + 2 * gap, rows * h * scale + (rows - 1) * gap
    out = bytearray(b"\x40" * (width * height * 3))
    frames = []
    for n in range(count):
        offset = 16 + n * (4 + w * h * 2)
        frames.append(struct.unpack_from("<I", data, offset)[0])
        pixels = struct.unpack_from(f"<{w * h}H", data, offset + 4)
        x0, y0 = (n % 3) * (w * scale + gap), (n // 3) * (h * scale + gap)
        for y in range(h * scale):
            line = bytearray()
            for x in range(w):
                c = pixels[(y // scale) * w + x]
                r, g, b = (c >> 11) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3
                line += bytes((r | r >> 5, g | g >> 6, b | b >> 5)) * scale
            start = ((y0 + y) * width + x0) * 3
            out[start:start + len(line)] = line
    write_png(path, width, height, bytes(out))
    print(f"{os.path.basename(path)}: frames " + ", ".join(
        f"{frames[i]} {frames[i + 1]} {frames[i + 2]}" for i in range(0, count - 2, 3)))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("issue", nargs="?", help="issue number or its URL")
    parser.add_argument("--out", help="output directory (default: bugreport_<number>)")
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument("--api", default="https://api.github.com")
    parser.add_argument("--list", action="store_true", help="list the open bug reports")
    args = parser.parse_args()

    if args.list:
        for issue in get_all(args.api, f"/repos/{args.repo}/issues?labels={LABEL}&state=open"):
            print(f"#{issue['number']}  {issue['created_at']}  {issue['title']}")
        return
    if not args.issue:
        parser.error("give an issue number, or --list")
    match = re.search(r"/issues/(\d+)", args.issue) or re.fullmatch(r"\s*#?(\d+)\s*", args.issue)
    if not match:
        parser.error(f"not an issue number or URL: {args.issue}")
    number = match.group(1)
    issue = get(args.api, f"/repos/{args.repo}/issues/{number}")
    comments = get_all(args.api, f"/repos/{args.repo}/issues/{number}/comments")
    out = args.out or f"bugreport_{number}"
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "issue.md"), "w") as f:
        f.write(f"# {issue['title']}\n\n{issue.get('body') or ''}\n")
    for name, data in attachments(comments, (issue.get("user") or {}).get("login")).items():
        # The names come from the console; keep them inside the output directory.
        with open(os.path.join(out, os.path.basename(name)), "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} bytes")
        if name.endswith("_flash.bin"):
            flash_to_png(data, os.path.join(out, os.path.basename(name)[:-4] + ".png"))
    print(f"saved to {out}/")


if __name__ == "__main__":
    main()
