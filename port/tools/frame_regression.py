#!/usr/bin/env python3
"""
Frame regression test for the host build: plays the same scripted scenarios on two builds and compares the frames.

Both builds run in lockstep (DKR_LOCKSTEP=1), which makes a run repeatable: the same build always draws the same
frames, so any difference between the two builds comes from the code that changed between them.

Usage: frame_regression.py --rom baserom.z64 --head port/build/host/dkr [--base old/port/build/host/dkr] --out DIR

Without --base, or when the base build predates lockstep, the head build is still played through every scenario
(which catches crashes and hangs), but nothing is compared.

Writes DIR/report.md (also appended to $GITHUB_STEP_SUMMARY when set) and, for each frame that differs, an image
with the base frame, the head frame and the difference side by side. Exits with 1 when the head build crashes,
hangs or stops early, or when it isn't repeatable; with 2 when frames differ and --fail-on-diff is given.
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time

from PIL import Image, ImageChops

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))

# Each scenario is a set of settings for the host build (see port/README.md). "save" names a file in testdata/ that
# is used as the EEPROM save. Frames are written every "every" frames.
SCENARIOS = [
    {
        "name": "intro",
        "about": "Rare logo, intro cutscene, title screen and the first attract demo, no input",
        "frames": 1500,
        "every": 30,
        "env": {},
    },
    {
        "name": "levels",
        "about": "the attract demo loaded with a spread of hubs, races and bosses, AI driving",
        "frames": 0,  # Filled in from the level list.
        "every": 20,
        "env": {"DKR_WARP": "LEVELS", "DKR_WARP_SECONDS": "4"},
    },
    {
        "name": "adventure",
        "about": "a complete save, into Adventure mode and the Dino Domain hub",
        "frames": 1500,
        "every": 30,
        "save": "eeprom_complete.bin",
        "env": {"DKR_INPUT": "INPUT"},
    },
]

# Levels for the "levels" scenario (ASSET_LEVEL_* ids).
LEVELS = [0, 1, 2, 12, 13, 15, 16, 20, 24, 26, 28, 29]
FRAMES_PER_LEVEL = 4 * 30 + 30  # DKR_WARP_SECONDS at 30 frames a second, plus loading.

# Presses START on the title screen, then A every 60 frames: past the menus into the Adventure hub.
ADVENTURE_INPUT = "700:START 706: " + " ".join("%d:A %d:" % (f, f + 6) for f in range(800, 1500, 60))

TIMEOUT_SECONDS = 900


def prepare_scenarios():
    for scenario in SCENARIOS:
        env = scenario["env"]
        if env.get("DKR_WARP") == "LEVELS":
            env["DKR_WARP"] = ",".join(str(level) for level in LEVELS)
            scenario["frames"] = 450 + FRAMES_PER_LEVEL * len(LEVELS)
        if env.get("DKR_INPUT") == "INPUT":
            env["DKR_INPUT"] = ADVENTURE_INPUT


def supports_lockstep(exe):
    with open(exe, "rb") as f:
        return b"DKR_LOCKSTEP" in f.read()


def run(exe, rom, scenario, out_dir):
    """Plays one scenario. Returns (ok, message, seconds)."""
    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    frames_dir = os.path.join(out_dir, "frames")
    os.makedirs(frames_dir)
    os.symlink(os.path.abspath(rom), os.path.join(out_dir, "baserom.z64"))
    if "save" in scenario:
        shutil.copy(os.path.join(TOOLS_DIR, "testdata", scenario["save"]), os.path.join(out_dir, "eeprom.bin"))

    env = dict(os.environ)
    env.update(scenario["env"])
    env.update({
        "DKR_LOCKSTEP": "1",
        "DKR_DATA_DIR": out_dir,
        "DKR_FRAMES": str(scenario["frames"]),
        "DKR_DUMP_EVERY": str(scenario["every"]),
        "DKR_DUMP_DIR": frames_dir,
    })
    start = time.time()
    with open(os.path.join(out_dir, "log.txt"), "w") as log:
        try:
            result = subprocess.run([os.path.abspath(exe)], env=env, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            return False, "did not finish within %d s" % TIMEOUT_SECONDS, time.time() - start
    seconds = time.time() - start
    if result.returncode != 0:
        return False, "exited with code %d" % result.returncode, seconds
    # The frame drawn as the build exits may or may not get written, so frames from that point on are dropped.
    for name in os.listdir(frames_dir):
        if int(name[len("frame_"):-len(".png")]) >= scenario["frames"]:
            os.remove(os.path.join(frames_dir, name))
    expected = (scenario["frames"] - 1) // scenario["every"]
    written = len(os.listdir(frames_dir))
    if written < expected:
        return False, "wrote %d of %d frames" % (written, expected), seconds
    return True, "", seconds


def frame_names(run_dir):
    return sorted(os.listdir(os.path.join(run_dir, "frames")))


def compare_runs(base_dir, head_dir, images_dir, prefix, max_images):
    """Returns the names of the frames that differ, writing side by side images of the first max_images."""
    differing = []
    for name in frame_names(head_dir):
        base_path = os.path.join(base_dir, "frames", name)
        head_path = os.path.join(head_dir, "frames", name)
        if not os.path.exists(base_path):
            differing.append(name)
            continue
        base = Image.open(base_path).convert("RGB")
        head = Image.open(head_path).convert("RGB")
        if base.size != head.size:
            differing.append(name)
            continue
        diff = ImageChops.difference(base, head)
        if diff.getbbox() is None:
            continue
        differing.append(name)
        if images_dir is not None and len(differing) <= max_images:
            # Amplify the difference so that small changes are visible.
            diff = diff.point(lambda v: min(255, v * 4))
            w, h = head.size
            side = Image.new("RGB", (w * 3, h))
            side.paste(base, (0, 0))
            side.paste(head, (w, 0))
            side.paste(diff, (w * 2, 0))
            os.makedirs(images_dir, exist_ok=True)
            side.save(os.path.join(images_dir, "%s_%s" % (prefix, name)))
    return differing


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", required=True)
    parser.add_argument("--head", required=True, help="host build under test")
    parser.add_argument("--base", help="host build to compare with")
    parser.add_argument("--out", required=True)
    parser.add_argument("--only", help="comma separated scenario names to run")
    parser.add_argument("--max-images", type=int, default=12, help="side by side images per scenario")
    parser.add_argument("--no-repeat-check", action="store_true",
                        help="skip playing the first scenario twice on the head build")
    parser.add_argument("--fail-on-diff", action="store_true")
    args = parser.parse_args()

    prepare_scenarios()
    scenarios = SCENARIOS
    if args.only:
        names = args.only.split(",")
        scenarios = [s for s in SCENARIOS if s["name"] in names]
    os.makedirs(args.out, exist_ok=True)
    runs_dir = os.path.join(args.out, "runs")
    images_dir = os.path.join(args.out, "differences")

    if not supports_lockstep(args.head):
        print("The head build has no DKR_LOCKSTEP support", file=sys.stderr)
        return 1
    compare = args.base is not None
    note = ""
    if compare and not supports_lockstep(args.base):
        compare = False
        note = "The base build predates lockstep mode, so its frames can't be compared. The head build was played " \
               "through every scenario on its own."
    elif not compare:
        note = "No base build given; the head build was played through every scenario on its own."

    lines = ["## Host frame regression", ""]
    if note:
        lines += [note, ""]
    if compare:
        lines += ["| Scenario | Frames compared | Differing | First difference | Time (base / head) |",
                  "| --- | --- | --- | --- | --- |"]
    else:
        lines += ["| Scenario | Frames | Time |", "| --- | --- | --- |"]

    failures = []
    total_differing = 0
    for scenario in scenarios:
        name = scenario["name"]
        print("Playing %s (%s)" % (name, scenario["about"]), flush=True)
        head_dir = os.path.join(runs_dir, name, "head")
        base_dir = os.path.join(runs_dir, name, "base")
        # Each run keeps about one core busy, so the two builds play side by side.
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            head_future = pool.submit(run, args.head, args.rom, scenario, head_dir)
            base_future = pool.submit(run, args.base, args.rom, scenario, base_dir) if compare else None
            ok, message, head_seconds = head_future.result()
            base_result = base_future.result() if base_future is not None else None
        if not ok:
            failures.append("%s: the head build %s (see runs/%s/head/log.txt)" % (name, message, name))
            lines.append("| %s | | | head build %s | |" % (name, message) if compare else
                         "| %s | head build %s | |" % (name, message))
            continue
        count = len(frame_names(head_dir))
        if not compare:
            lines.append("| %s | %d | %.0f s |" % (name, count, head_seconds))
            continue
        ok, message, base_seconds = base_result
        if not ok:
            lines.append("| %s | | | base build %s, not compared | |" % (name, message))
            continue
        differing = compare_runs(base_dir, head_dir, images_dir, name, args.max_images)
        total_differing += len(differing)
        lines.append("| %s | %d | %d | %s | %.0f s / %.0f s |" % (
            name, count, len(differing), differing[0] if differing else "", base_seconds, head_seconds))

    if not args.no_repeat_check and scenarios and not failures:
        # A second run of the same build must draw exactly the same frames, or no comparison means anything.
        scenario = scenarios[0]
        first_dir = os.path.join(runs_dir, scenario["name"], "head")
        again_dir = os.path.join(runs_dir, scenario["name"], "head_again")
        ok, message, _ = run(args.head, args.rom, scenario, again_dir)
        repeat = compare_runs(first_dir, again_dir, None, "", 0) if ok else ["(run failed: %s)" % message]
        if repeat:
            failures.append("%s: two runs of the head build differ in %d frames, so lockstep isn't repeatable" %
                            (scenario["name"], len(repeat)))
        else:
            lines += ["", "Repeat check: a second run of `%s` on the head build drew identical frames." %
                      scenario["name"]]

    lines.append("")
    if failures:
        lines += ["**Failed:**", ""] + ["- " + f for f in failures] + [""]
    if compare:
        if total_differing:
            if args.max_images > 0:
                lines.append("%d frames differ from the base build. Side by side images (base, head, difference x4) "
                             "are in %s." % (total_differing, os.path.join(args.out, "differences")))
            else:
                lines.append("%d frames differ from the base build. Run port/tools/frame_regression.py locally to "
                             "see them side by side." % total_differing)
        else:
            lines.append("Every frame matches the base build.")
    report = "\n".join(lines) + "\n"
    with open(os.path.join(args.out, "report.md"), "w") as f:
        f.write(report)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(report)
    print(report)

    if failures:
        return 1
    if total_differing and args.fail_on_diff:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
