#!/bin/bash
# Captures frames of one model with the portrait camera (port/src/game/port_portrait.c) on the PC build.
#   cap.sh name dist height yaw [VAR=value ...]
# Run it from a folder holding baserom.z64; frames land in ./name/frame_NNNNN.png every 50 frames up to 1600.
R=$PWD
GAME=$(cd "$(dirname "$0")/../../build/host" && pwd)/dkr
n=$1; d=$2; h=$3; y=$4; shift 4
rm -rf "$R/$n"; mkdir -p "$R/$n/d"; ln -s "$R/baserom.z64" "$R/$n/d/baserom.z64"
cd "$R/$n" && timeout 250 env DKR_DATA_DIR=d DKR_DUMP_DIR=. DKR_DUMP_EVERY=50 SDL_VIDEODRIVER=dummy \
    DKR_PORTRAIT_DIST=$d DKR_PORTRAIT_HEIGHT=$h DKR_PORTRAIT_YAW=$y DKR_FRAMES=1600 "$@" "$GAME" >log.txt 2>&1
