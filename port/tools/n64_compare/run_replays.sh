#!/bin/bash
# Replays every captured frame of a capture directory and compares them.
#
#   run_replays.sh <capture_dir> <out_dir> [replay_binary]
#
# Each task_N.rdram.gz is paired with the RDP full sync seen while task N (or, failing that, N+1) was the latest
# graphics task started, taking the one whose colour image is the task's.
set -e
D=$1
O=$2
R=${3:-$(dirname "$0")/build/replay}
mkdir -p "$O"
for f in "$D"/task_*.rdram.gz; do
    t=$(basename "$f" | sed 's/task_\([0-9]*\).*/\1/')
    n=$(printf "%06d" $((10#$t + 1)))
    echo "$t $(ls "$D"/sync_${t}_*.fb "$D"/sync_${n}_*.fb 2>/dev/null | tr '\n' ' ' | sed 's/ *$//')"
done > "$O/jobs.txt"
xargs -P "$(nproc)" -L 1 sh -c '
t=$0
for fb in "$@"; do
    "'"$R"'" "'"$D"'/task_$t.rdram.gz" "$fb" "'"$O"'/$t" > /dev/null 2> "'"$O"'/$t.log" || true
    grep -q "note: RDP colour image" "'"$O"'/$t.log" || break
done' < "$O/jobs.txt"
python3 "$(dirname "$0")/compare.py" "$O" 10
