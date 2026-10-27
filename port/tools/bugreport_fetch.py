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
higher rate limit. Snapshots (diff_N_ref.png + diff_N_gpu.bin) can then be compared with diff_frames.py.
"""
import argparse
import base64
import gzip
import json
import os
import re
import sys
import urllib.request

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
    print(f"saved to {out}/")


if __name__ == "__main__":
    main()
