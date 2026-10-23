#!/bin/bash
# Checks the port's renderer against a real N64: ares (low level RSP running the game's own F3DDKR microcode, and
# paraLLEl-RDP) runs the ROM, saves chosen frames' display lists and finished pictures, and the port's renderer
# replays the same display lists. See README.md.
#
#   n64_compare.sh setup                    builds ares-headless and the replay tool (Ubuntu 24.04, needs sudo)
#   n64_compare.sh capture attract <dir>    intro, character intros and the title screen over many tracks (~25 min)
#   n64_compare.sh capture race <dir>       menus and a Tracks race on Ancient Lake with the HUD (~25 min)
#   n64_compare.sh replay <dir> <out>       replays and compares a capture: <out>/report.csv and side by sides
#
# WORK (default ~/n64_compare) holds the ares source and build; ROM (default baseroms/baserom.us.v77.z64) is the
# US 1.0 ROM.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
WORK=${WORK:-$HOME/n64_compare}
ROM=${ROM:-$ROOT/baseroms/baserom.us.v77.z64}
ARES=$WORK/ares-134+dfsg
HEADLESS=$ARES/headless/out/ares-headless

case "$1" in
setup)
    # Ubuntu's ares package source: its N64 core has the hooks we need and builds without the GUI.
    sudo sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
    sudo apt-get update -qq
    sudo apt-get install -y -qq dpkg-dev mesa-vulkan-drivers libvulkan-dev libchdr-dev zlib1g-dev gcc-multilib \
        python3-pil python3-numpy
    mkdir -p "$WORK"
    if [ ! -d "$ARES" ]; then
        (cd "$WORK" && apt-get source ares=134+dfsg-1build2)
        (cd "$ARES" && patch -p1 < "$HERE/ares-hooks.patch")
    fi
    mkdir -p "$ARES/headless"
    cp "$HERE/headless/headless.cpp" "$HERE/headless/GNUmakefile" "$ARES/headless/"
    make -C "$ARES/headless" -j"$(nproc)"
    make -C "$ROOT/port" TARGET=host -j"$(nproc)"
    make -C "$HERE"
    ;;
capture)
    OUT=$3
    mkdir -p "$OUT"
    case "$2" in
    attract)
        # Every 40th graphics task up to the end of the title screen's demos.
        "$HEADLESS" --rom "$ROM" --out "$OUT" --frames 9700 --dump-every 40 --dump-from 40
        ;;
    race)
        # Title > player select (OK) > caution > Tracks > Ancient Lake > car > T.T. off > race, driving with A and a
        # little steering. Times are in video frames.
        S="6000:START 6006: 6300:START 6306: 6600:START 6606: 6900:START 6906: 7050:STICK=0,-80 7058: 7100:A 7106:"
        S="$S 7400:A 7406: 7700:A 7706: 8000:A 8006: 8300:A 8306: 8700:A 9300:A+STICK=-40,0 9400:A"
        S="$S 9800:A+STICK=40,0 9900:A 10400:A+Z 10500:A"
        "$HEADLESS" --rom "$ROM" --out "$OUT" --frames 11500 --script "$S" --dump-every 20 --dump-from 3000
        ;;
    *)
        echo "unknown capture '$2'" >&2
        exit 1
        ;;
    esac
    ;;
replay)
    "$HERE/run_replays.sh" "$2" "$3" "$HERE/build/replay"
    ;;
*)
    sed -n '2,15p' "$0"
    exit 1
    ;;
esac
