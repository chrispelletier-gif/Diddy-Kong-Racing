#!/usr/bin/env python3
"""
Clipping sweep: races every track on the host build with the AI driving (the 3DS test run's autorun), in each screen
mode, and finds the frames where the camera ends up against or inside scenery, where the sky (or anything) leaves a
hole, and where the 3DS depth buffer would show a different surface than exact depth would.

The host renderer measures each frame (DKR_CLIP_SWEEP, host/gfx_soft.c) and writes the suspicious ones to PNG files,
with a second copy marking the problem pixels: magenta where nothing 3D was drawn, red where the level's opaque
geometry is closer than 30 units to the camera (the kart is 120 to 190 away, so that is a wall or scenery between
them), yellow where an object (another kart) is that close, green where the camera is behind a close surface of the
level (inside a wall, a rock or the ground, so the view goes through it), cyan where the 3DS depth test (24 or 16 bit,
DKR_DEPTH_BITS) left another triangle showing than float depth.

Usage: clip_sweep.py <host dkr> <baserom.z64> <out dir> [--tracks 3,4,...] [--modes 43,balanced,wide]
                     [--jobs 4] [--character 1] [--walls]
       clip_sweep.py --report <out dir>

Each run stops when its race is over (or after --max-frames; a hub or arena after WARP_SECONDS of the attract demo). The report groups consecutive flagged frames into
events, and for each event keeps the worst frame, side by side with its marked copy, in <out dir>/events/.
"""
import argparse
import csv
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

# Each world's four tracks, as the tracks menu (ASSET_LEVEL_* ids).
TRACKS = {
    5: "Ancient Lake", 3: "Fossil Canyon", 29: "Jungle Falls", 7: "Hot Top Volcano",
    20: "Windmill Plains", 18: "Greenwood Village", 19: "Boulder Canyon", 31: "Haunted Woods",
    13: "EverFrost Peak", 6: "Walrus Cove", 9: "Snowball Valley", 28: "Frosty Village",
    8: "Whale Bay", 10: "Crescent Island", 4: "Pirate Lagoon", 30: "Treasure Caves",
    15: "Spaceport Alpha", 17: "Spacedust Alley", 32: "Darkmoon Caverns", 33: "Star City",
}

# Boss fights (raced as the test run's boss mode) and the levels without a race (hubs, battle and challenge arenas,
# the trophy race), which the title screen's attract demo drives through instead (DKR_WARP).
BOSSES = {
    38: "Tricky 1", 1: "Bluey 1", 40: "Bubbler 1", 41: "Smokey 1", 37: "Wizpig 1",
    46: "Tricky 2", 52: "Bluey 2", 53: "Bubbler 2", 54: "Smokey 2", 55: "Wizpig 2",
}
WARPS = {
    0: "Central Area hub", 2: "Dragon Forest hub", 12: "Dino Domain hub", 14: "Sherbet Island hub",
    24: "Snowflake Mountain hub", 35: "Future Fun Land hub", 11: "Fire Mountain", 25: "Smokey Castle",
    26: "Darkwater Beach", 27: "Icicle Pyramid",
}
NAMES = {**TRACKS, **BOSSES, **WARPS}
WARP_SECONDS = 90

# Screen modes: the host's DKR_WIDESCREEN (0 4:3, 1 Wide, 2 Balanced, 3 Zoomed) and the depth buffer's bits (24, the
# 3DS's; its 16-bit buffers setting is gone).
MODES = {
    "43": {"DKR_WIDESCREEN": "0", "DKR_DEPTH_BITS": "24"},
    "balanced": {"DKR_WIDESCREEN": "2", "DKR_DEPTH_BITS": "24"},
    "wide": {"DKR_WIDESCREEN": "1", "DKR_DEPTH_BITS": "24"},
    "balanced60": {"DKR_WIDESCREEN": "2", "DKR_DEPTH_BITS": "24", "DKR_60FPS": "1"},
}

# What counts as a bad frame (shares of the screen), matching the dump thresholds in gfx_soft.c.
HOLES = 0.001
NEAR = 0.02
INSIDE = 0.01
DEPTH = 0.005


def run_one(exe, rom, out, mode, track, character, max_frames, walls):
    run_dir = os.path.join(out, mode, "%02d" % track)
    shutil.rmtree(run_dir, ignore_errors=True)
    os.makedirs(os.path.join(run_dir, "frames"))
    shutil.copy(rom, os.path.join(run_dir, "baserom.z64"))
    env = dict(os.environ)
    env.update(MODES[mode])
    env["DKR_CAMERA_WALLS"] = "1" if walls else "0"
    if MODES[mode].get("DKR_60FPS") == "1":
        max_frames *= 2
    if track in WARPS:
        env.update({"DKR_WARP": str(track), "DKR_WARP_SECONDS": str(WARP_SECONDS)})
        max_frames = min(max_frames, WARP_SECONDS * 30 + 900)
    elif track in BOSSES:
        env.update({"DKR_AUTORUN_BOSS": "1", "DKR_AUTORUN_BOSS_TRACKS": str(track),
                    "DKR_AUTORUN_BOSS_CHARACTER": str(character)})
    else:
        env.update({"DKR_AUTORUN": "1", "DKR_AUTORUN_TRACKS": str(track),
                    "DKR_AUTORUN_CHARACTERS": str(character)})
    env.update({
        "DKR_LOCKSTEP": "1",
        "DKR_DATA_DIR": run_dir,
        "DKR_FRAMES": str(max_frames),
        "DKR_CLIP_SWEEP": os.path.join(run_dir, "sweep.csv"),
        "DKR_CLIP_SWEEP_MASK": "1",
        "DKR_DUMP_DIR": os.path.join(run_dir, "frames"),
    })
    start = time.time()
    log_path = os.path.join(run_dir, "log.txt")
    with open(log_path, "w") as log:
        proc = subprocess.Popen([os.path.abspath(exe)], env=env, stdout=log, stderr=subprocess.STDOUT, cwd=run_dir)
        while proc.poll() is None:
            time.sleep(5)
            with open(log_path, errors="replace") as f:
                if "races done" in f.read():
                    proc.kill()
                    break
        proc.wait()
    os.remove(os.path.join(run_dir, "baserom.z64"))
    print("%s %s: %.0f s" % (mode, NAMES.get(track, track), time.time() - start), flush=True)


def events(rows):
    """Groups consecutive flagged race frames into events: [kind, first, last, worst score, worst row]."""
    found, current = [], {}
    for row in rows:
        holes, near, depth = float(row["holes"]), float(row["nearcut"]), float(row["depthdiff"])
        objects = float(row.get("nearobj") or 0)
        inside = float(row.get("inside") or 0)
        if holes >= 0.5:
            continue  # A loading or fade frame
        frame = int(row["frame"])
        for kind, score, limit in (("holes", holes, HOLES), ("near", near, NEAR), ("object", objects, NEAR),
                                   ("inside", inside, INSIDE), ("depth", depth, DEPTH)):
            if score <= limit:
                continue
            event = current.get(kind)
            if event and frame - event[2] <= 3:
                event[2] = frame
                if score > event[3]:
                    event[3], event[4] = score, row
            else:
                event = current[kind] = [kind, frame, frame, score, row]
                found.append(event)
    return found


def report(out):
    from PIL import Image
    events_dir = os.path.join(out, "events")
    shutil.rmtree(events_dir, ignore_errors=True)
    os.makedirs(events_dir)
    lines = ["mode,track,kind,first,last,frames,worst,score,camx,camy,camz,camseg,image"]
    for mode in sorted(os.listdir(out)):
        if mode not in MODES:
            continue
        for track_dir in sorted(os.listdir(os.path.join(out, mode))):
            run_dir = os.path.join(out, mode, track_dir)
            path = os.path.join(run_dir, "sweep.csv")
            if not os.path.exists(path):
                continue
            track = int(track_dir)
            with open(path) as f:
                rows = [r for r in csv.DictReader(f) if r.get("depthdiff") and int(r["level"]) == track]
            for kind, first, last, score, row in events(rows):
                worst = int(row["frame"])
                frame = os.path.join(run_dir, "frames", "sweep_%05d.png" % worst)
                mask = os.path.join(run_dir, "frames", "sweep_%05d_mask.png" % worst)
                image = ""
                if os.path.exists(frame) and os.path.exists(mask):
                    a, b = Image.open(frame).convert("RGB"), Image.open(mask).convert("RGB")
                    pair = Image.new("RGB", (a.width + b.width + 4, a.height), (255, 255, 255))
                    pair.paste(a, (0, 0))
                    pair.paste(b, (a.width + 4, 0))
                    image = "%s_%s_%s_%05d.png" % (mode, track_dir, kind, worst)
                    pair.save(os.path.join(events_dir, image))
                lines.append(",".join(str(v) for v in (
                    mode, NAMES.get(track, track), kind, first, last, last - first + 1, worst, "%.4f" % score,
                    row["camx"], row["camy"], row["camz"], row["camseg"], image)))
    with open(os.path.join(out, "events.csv"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%d events, listed in %s" % (len(lines) - 1, os.path.join(out, "events.csv")))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("exe", nargs="?")
    parser.add_argument("rom", nargs="?")
    parser.add_argument("out")
    parser.add_argument("--tracks", default=",".join(str(t) for t in TRACKS),
                        help="Level ids: race tracks, boss fights (%s) and hubs or arenas (%s); 'extras' for those"
                        % (",".join(str(t) for t in BOSSES), ",".join(str(t) for t in WARPS)))
    parser.add_argument("--modes", default="43,balanced,wide")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--character", type=int, default=1)
    parser.add_argument("--max-frames", type=int, default=7000)
    parser.add_argument("--walls", action="store_true", help="With the camera wall check on (off: the N64's camera)")
    parser.add_argument("--report", action="store_true", help="Only summarise an earlier sweep")
    args = parser.parse_args()
    if args.report:
        report(args.out if args.exe is None else args.exe)
        return
    if args.exe is None or args.rom is None:
        parser.error("the host build and the ROM are needed")
    tracks = args.tracks
    if tracks == "extras":
        tracks = ",".join(str(t) for t in list(BOSSES) + list(WARPS))
    jobs = [(m, int(t)) for m in args.modes.split(",") for t in tracks.split(",")]
    with ThreadPoolExecutor(args.jobs) as pool:
        for future in [pool.submit(run_one, args.exe, args.rom, args.out, m, t, args.character, args.max_frames,
                                   args.walls)
                       for m, t in jobs]:
            future.result()
    report(args.out)


if __name__ == "__main__":
    sys.exit(main())
