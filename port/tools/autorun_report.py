#!/usr/bin/env python3
"""Summarises the unattended test run's results (autorun_results.csv from the 3DS, or from a test-run issue saved
with bugreport_fetch.py) as a Markdown report.

    python3 port/tools/autorun_report.py autorun_results.csv [--run N] [--out report.md]

The anti-aliasing benchmark's aa_bench_results.csv (it has an "aa" column) also gets a table by track and AA setting.

Only finished races count towards the performance tables; every other outcome is listed under Problems.
"""
import argparse
import csv
import sys
from collections import defaultdict

STAGES = [
    ("logic_ms", "logic"),
    ("scene_ms", "scene"),
    ("rdp_ms", "rdp"),
    ("texture_ms", "tex"),
    ("gpu_draw_ms", "gpu"),
    ("audio_ms", "audio"),
]


def num(row, key):
    try:
        return float(row.get(key) or 0)
    except ValueError:
        return 0.0


def mean(values):
    values = list(values)
    return sum(values) / len(values) if values else 0.0


def group_table(rows, key, title):
    groups = defaultdict(list)
    for row in rows:
        groups[row[key]].append(row)
    lines = [
        f"## By {title}",
        "",
        f"| {title} | races | fps | 30 fps | 20 fps | worst frame | hitches/race | "
        + " | ".join(label for _, label in STAGES)
        + " | limit |",
        "|---|---:|---:|---:|---:|---:|---:|" + "---:|" * len(STAGES) + "---|",
    ]
    for name, group in sorted(groups.items(), key=lambda item: mean(num(r, "fps") for r in item[1])):
        limits = defaultdict(int)
        for row in group:
            limits[row["limit"]] += 1
        limit = max(limits, key=limits.get)
        lines.append(
            f"| {name} | {len(group)} | {mean(num(r, 'fps') for r in group):.1f} | "
            f"{mean(num(r, 'pct_30fps') for r in group):.0f}% | {mean(num(r, 'pct_20fps') for r in group):.0f}% | "
            f"{max(num(r, 'worst_frame_ms') for r in group):.0f} ms | "
            f"{mean(num(r, 'hitches') for r in group):.1f} | "
            + " | ".join(f"{mean(num(r, key) for r in group):.1f}" for key, _ in STAGES)
            + f" | {limit} |"
        )
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", nargs="+")
    parser.add_argument("--run", type=int, help="only this run (default: the latest)")
    parser.add_argument("--out", help="write the report here instead of printing it")
    args = parser.parse_args()

    rows = []
    for path in args.csv:
        with open(path, newline="") as f:
            rows.extend(csv.DictReader(f))
    if not rows:
        sys.exit("no results")
    run = args.run if args.run is not None else max(int(r["run"]) for r in rows)
    rows = [r for r in rows if int(r["run"]) == run]
    # A race that was repeated (a resumed run) counts once: its last line wins.
    latest = {}
    for row in rows:
        latest[row["race"]] = row
    rows = sorted(latest.values(), key=lambda r: int(r["race"]))
    finished = [r for r in rows if r["outcome"] == "finished"]
    problems = [r for r in rows if r["outcome"] != "finished"]

    lines = [
        f"# Test run {run}",
        "",
        f"{len(rows)} races, {len(finished)} finished, {len(problems)} with problems.",
        "",
    ]
    if finished:
        lines += [
            f"Over the finished races: {mean(num(r, 'fps') for r in finished):.1f} fps on average, "
            f"{mean(num(r, 'pct_30fps') for r in finished):.0f}% of frames at 30 fps, "
            f"{sum(num(r, 'hitches') for r in finished):.0f} frames over 50 ms in all.",
            "",
            "Average milliseconds per frame: "
            + ", ".join(f"{label} {mean(num(r, key) for r in finished):.2f}" for key, label in STAGES)
            + ". A frame at 30 fps has 33.3 ms; the game thread (logic + scene), the renderer (rdp + tex) and the GPU "
            "work in parallel, so the largest of the three sets the frame rate.",
            "",
        ]
        lines += group_table(finished, "track", "track") + [""]
        if any(r.get("aa") not in (None, "") for r in finished):
            for row in finished:
                row["track and AA"] = f"{row['track']}, AA {'on' if row.get('aa') == '1' else 'off'}"
            lines += group_table(finished, "track and AA", "track and AA") + [""]
        lines += group_table(finished, "character", "character") + [""]
        lines += ["## Slowest races", "", "| race | track | character | vehicle | fps | worst frame | limit |",
                  "|---:|---|---|---|---:|---:|---|"]
        for row in sorted(finished, key=lambda r: num(r, "fps"))[:10]:
            lines.append(f"| {row['race']} | {row['track']} | {row['character']} | {row['vehicle']} | "
                         f"{num(row, 'fps'):.1f} | {num(row, 'worst_frame_ms'):.0f} ms | {row['limit']} |")
        lines.append("")
    lines += ["## Problems", ""]
    if problems:
        lines += ["| race | track | character | vehicle | outcome | after |", "|---:|---|---|---|---|---:|"]
        for row in problems:
            lines.append(f"| {row['race']} | {row['track']} | {row['character']} | {row['vehicle']} | "
                         f"{row['outcome']} | {num(row, 'race_s'):.0f} s |")
    else:
        lines.append("None.")
    text = "\n".join(lines) + "\n"
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
