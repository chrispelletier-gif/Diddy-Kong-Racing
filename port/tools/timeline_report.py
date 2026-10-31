#!/usr/bin/env python3
"""
Reads a flight recorder timeline (timeline.csv or last_timeline.csv from a 3DS bug report, see
port/src/plat/recorder.h) and prints what happened around the bug report shortcut: the frames and events in a window
around it, and the slowest frames of the whole recording with what took their time.

    python3 port/tools/timeline_report.py bugreport_275/timeline.csv
    python3 port/tools/timeline_report.py timeline.csv --before 3 --after 1   # seconds around the press
    python3 port/tools/timeline_report.py timeline.csv --all                 # every row, not just the window

Times are relative to the press (rel_ms), or to the last frame when the recording has no press (a crash or a hang).
"""
import argparse
import csv
import sys

STAGES = ["logic_ms", "scene_ms", "render_wait_ms", "vsync_ms"]


def number(row, key):
    try:
        return float(row.get(key) or 0)
    except ValueError:
        return 0.0


def cause(row):
    """The biggest part of a frame's time, as the recorder's summary names it."""
    stage = max(STAGES, key=lambda k: number(row, k))
    if stage == "logic_ms":
        return f"game logic {number(row, 'logic_ms'):.1f}"
    if stage == "scene_ms":
        return f"game thread (scene, loads) {number(row, 'scene_ms'):.1f}"
    if stage == "vsync_ms":
        return f"retrace {number(row, 'vsync_ms'):.1f}"
    return (f"renderer wait {number(row, 'render_wait_ms'):.1f} (rdp {number(row, 'rdp_ms'):.1f}, "
            f"tex {number(row, 'tex_ms'):.1f}/{row.get('tex_decodes')}, gpu wait {number(row, 'gpu_wait_ms'):.1f}, "
            f"gpu {number(row, 'gpu_ms'):.1f})")


def frame_line(row):
    flags = int(row.get("flags") or 0)
    marks = ("H" if flags & 1 else " ") + (">" if flags & 2 else " ")
    where = f"{row['mode']} lvl {row['level']} menu {row['menu']}" + (" paused" if row.get("paused") == "1" else "")
    if row.get("racing") == "1":
        where += f" lap {row['lap']} cp {row['cp']}"
    return (f"{marks} {int(row['rel_ms']):>7} ms  frame {row['frame']:>6} (r{row['render_frame']:>6})  "
            f"{number(row, 'period_ms'):6.1f} ms  logic {number(row, 'logic_ms'):5.1f} scene {number(row, 'scene_ms'):5.1f} "
            f"rdp {number(row, 'rdp_ms'):5.1f} tex {number(row, 'tex_ms'):5.1f} gpu {number(row, 'gpu_ms'):5.1f}  "
            f"{row['tris']:>5} tris {row['draws']:>4} draws  {where}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv")
    parser.add_argument("--before", type=float, default=2.0, help="seconds before the press to show (default 2)")
    parser.add_argument("--after", type=float, default=0.5, help="seconds after the press to show (default 0.5)")
    parser.add_argument("--all", action="store_true", help="show every row")
    parser.add_argument("--worst", type=int, default=10, help="how many of the slowest frames to list")
    args = parser.parse_args()

    with open(args.csv, newline="") as f:
        header = [line for line in f if line.startswith("#")]
    with open(args.csv, newline="") as f:
        rows = list(csv.DictReader(line for line in f if not line.startswith("#")))
    for line in header:
        print(line.rstrip())
    frames = [r for r in rows if r["kind"] == "f"]
    if not frames:
        print("no frames recorded")
        return 1

    print(f"\n{len(frames)} frames, {len(rows) - len(frames)} events, "
          f"{sum(1 for r in frames if int(r['flags'] or 0) & 1)} hitches (H); > marks the press\n")
    low, high = -args.before * 1000, args.after * 1000
    for row in rows:
        rel = int(row["rel_ms"])
        if not args.all and not low <= rel <= high:
            continue
        if row["kind"] == "f":
            print(frame_line(row))
        else:
            print(f"   {rel:>7} ms  event: {row['text']}")

    print(f"\nSlowest {args.worst} frames:")
    events = [r for r in rows if r["kind"] == "e"]
    for row in sorted(frames[1:], key=lambda r: -number(r, "period_ms"))[:args.worst]:
        end = int(row["t_ms"])
        start = end - number(row, "period_ms")
        during = [e["text"] for e in events if start - 5 <= int(e["t_ms"]) <= end + 5
                  and not e["text"].startswith("watchdog")]
        print(f"  frame {row['frame']} at {int(row['rel_ms']) / 1000:+.2f} s: {number(row, 'period_ms'):.1f} ms, "
              f"{cause(row)}" + (f"; during: {' | '.join(during[:4])}" if during else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
