#!/bin/bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# A long share, with memory sampled throughout, to find leaks that only show over hours.
#
# RSS alone is not proof of a leak - an allocator holds freed pages - so this records RSS, the heap
# sections from /proc/<pid>/smaps_rollup, the open file descriptor count and the thread count, and the
# client's own stats lines. A leak shows as a line with a slope that does not flatten.
#
#   soak.sh <minutes> <outdir>
set -u
# shellcheck source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

# A fresh network namespace's loopback is DOWN, and everything here talks to 127.0.0.1. Without
# this the clients start, never reach the server, and what gets profiled or soaked is two clients
# retrying a connection - which is a measurement of nothing that looks like a measurement of
# something. This line was missing from both of these scripts for their whole existence.
ip link set lo up
MINUTES="${1:-120}"
OUT="${2:-$MV_OUT/soak}"


mkdir -p "$OUT"
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/soak.log"; }

PIDS=()
cleanup() {
    for d in "$MV_WORK/clients/sender" "$MV_WORK/clients/receiver"; do
        HOME="$d" XDG_RUNTIME_DIR="$d/run" timeout 10 "$MV_CLIENT" rpc quit >/dev/null 2>&1
    done
    sleep 2
    for p in ${PIDS[@]+"${PIDS[@]}"}; do kill "$p" 2>/dev/null; done
    sleep 1
    for p in ${PIDS[@]+"${PIDS[@]}"}; do kill -9 "$p" 2>/dev/null; done
}
trap cleanup EXIT

Xvfb :90 -screen 0 1920x1080x24 -extension GLX >/dev/null 2>&1 & PIDS+=($!)
Xvfb :91 -screen 0 1920x1080x24 -extension GLX >/dev/null 2>&1 & PIDS+=($!)
sleep 2

"$MV_SERVER" --foreground --ini "$MV_WORK/server/mumble-server.ini" > "$OUT/server.stdout" 2>&1 &
SERVER_PID=$!
PIDS+=("$SERVER_PID")
sleep 3

start_client() {
    local name="$1" display="$2" user="$3"
    shift 3
    local dir="$MV_WORK/clients/$name"
    mkdir -p "$dir/run"; chmod 700 "$dir/run"
    rm -f "$dir/run/MumbleSocket" "$dir/mumble.lock"
    env HOME="$dir" XDG_RUNTIME_DIR="$dir/run" XDG_CONFIG_HOME="$dir/.config" \
        XDG_DATA_HOME="$dir/.local/share" DISPLAY="$display" QT_QPA_PLATFORM=xcb \
        MUMBLE_VIDEO_STATS=1 "$@" \
        "$MV_CLIENT" --skip-settings-backup-prompt "mumble://$user@127.0.0.1:64740/" \
        > "$OUT/$name.log" 2>&1 &
    echo $!
}

SENDER_PID=$(start_client sender :90 qa-mv1 env MUMBLE_MOCK_SCREEN=1 MUMBLE_MOCK_SCREEN_SIZE=1920x1080)
PIDS+=("$SENDER_PID")
RECEIVER_PID=$(start_client receiver :91 qa-mv2 env)
PIDS+=("$RECEIVER_PID")
sleep 8

rpc() { local d="$MV_WORK/clients/$1"; shift; HOME="$d" XDG_RUNTIME_DIR="$d/run" timeout 10 "$MV_CLIENT" rpc "$@" >/dev/null 2>&1; }
for _ in $(seq 1 30); do rpc sender sharescreen; sleep 2; grep -q "VideoStats tx" "$OUT/sender.log" && break; done
for _ in $(seq 1 30); do rpc receiver watchall; sleep 2; grep -q "VideoStats rx" "$OUT/receiver.log" && break; done

if ! grep -q "VideoStats rx" "$OUT/receiver.log"; then
    say "never got going; aborting"
    exit 1
fi
say "soaking for ${MINUTES} minutes"

echo "epoch,proc,rss_kb,anon_kb,fds,threads" > "$OUT/memory.csv"
sample() {
    local label="$1" pid="$2" now="$3"
    [ -d "/proc/$pid" ] || return
    local rss anon fds threads
    rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)
    anon=$(awk '/^Anonymous:/{print $2}' "/proc/$pid/smaps_rollup" 2>/dev/null)
    fds=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)
    threads=$(awk '/Threads/{print $2}' "/proc/$pid/status" 2>/dev/null)
    echo "$now,$label,${rss:-0},${anon:-0},$fds,${threads:-0}" >> "$OUT/memory.csv"
}

END=$(( $(date +%s) + MINUTES * 60 ))
while [ "$(date +%s)" -lt "$END" ]; do
    NOW=$(date +%s)
    sample server "$SERVER_PID" "$NOW"
    sample sender "$SENDER_PID" "$NOW"
    sample receiver "$RECEIVER_PID" "$NOW"
    sleep 15
done

say "soak finished; fitting a slope per process"
$MV_PYTHON - "$OUT/memory.csv" <<'PY'
import csv, sys
import numpy as np

rows = list(csv.DictReader(open(sys.argv[1])))
print(f"{'proc':10} {'samples':>8} {'first MB':>9} {'last MB':>9} {'MB/hour':>9} {'fds':>6} {'threads':>8}")
for name in sorted({r["proc"] for r in rows}):
    mine = [r for r in rows if r["proc"] == name]
    t = np.array([float(r["epoch"]) for r in mine])
    rss = np.array([float(r["rss_kb"]) for r in mine]) / 1024.0
    # Over the last two thirds only: the first third is start-up, where growth is expected and means
    # nothing about a leak.
    cut = len(t) // 3
    slope = np.polyfit(t[cut:], rss[cut:], 1)[0] * 3600.0 if len(t) - cut > 2 else float("nan")
    print(f"{name:10} {len(mine):8} {rss[0]:9.1f} {rss[-1]:9.1f} {slope:9.2f} "
          f"{mine[-1]['fds']:>6} {mine[-1]['threads']:>8}")
PY
