#!/bin/bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# CPU profiles of the sender and the receiver while a share is running, as perf reports and folded
# stacks ready for a flame graph.
#
# perf_event_paranoid is 2 on this host, so only user-space samples are available - no kernel stacks,
# no scheduler tracing. That is enough for "which of our functions is hot", which is the question, and
# it needs no root.
#
#   profile.sh <seconds> <outdir>
set -u
# shellcheck source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

# A fresh network namespace's loopback is DOWN, and everything here talks to 127.0.0.1. Without
# this the clients start, never reach the server, and what gets profiled or soaked is two clients
# retrying a connection - which is a measurement of nothing that looks like a measurement of
# something. This line was missing from both of these scripts for their whole existence.
ip link set lo up
SECONDS_TOTAL="${1:-25}"
OUT="${2:-$MV_OUT/profile}"


mkdir -p "$OUT"
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/profile.log"; }

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
grep -m1 "Screen share:" "$OUT/sender.log" | tee -a "$OUT/profile.log"

# Checked, not assumed. A profile of a pipeline that is not running is not an empty result - it is a
# plausible-looking one, full of connection retries and audio threads, and it was reported as a
# profile of the video pipeline for exactly as long as nobody checked.
if ! grep -q "VideoStats tx" "$OUT/sender.log" || ! grep -q "VideoStats rx" "$OUT/receiver.log"; then
    say "no frames are moving; there is nothing worth profiling. Last lines of each client:"
    tail -15 "$OUT/sender.log" | tee -a "$OUT/profile.log"
    tail -15 "$OUT/receiver.log" | tee -a "$OUT/profile.log"
    exit 1
fi

say "both running; settling"
sleep 8

# -g for call graphs; dwarf unwinding because Qt and FFmpeg are built without frame pointers, and
# without it every stack stops at the first library.
for pair in "sender:$SENDER_PID" "receiver:$RECEIVER_PID"; do
    name="${pair%%:*}"
    pid="${pair##*:}"
    say "profiling $name ($pid) for ${SECONDS_TOTAL}s"
    perf record -F 499 -g --call-graph dwarf,16384 -p "$pid" -o "$OUT/$name.data" \
        -- sleep "$SECONDS_TOTAL" > "$OUT/$name.perf.log" 2>&1
    say "  perf record exit $?"
done

say "reporting"
for name in sender receiver; do
    [ -f "$OUT/$name.data" ] || continue
    perf report -i "$OUT/$name.data" --stdio --no-children --percent-limit 0.4 \
        > "$OUT/$name.flat.txt" 2>/dev/null
    perf report -i "$OUT/$name.data" --stdio --children --percent-limit 1.0 \
        > "$OUT/$name.tree.txt" 2>/dev/null
    # Folded stacks: one line per unique stack with a count, which is what a flame graph renders and is
    # also readable on its own with sort/head.
    perf script -i "$OUT/$name.data" 2>/dev/null \
        | awk '
            /^$/ { if (n > 0) { s = ""; for (i = n; i >= 1; i--) s = s (s == "" ? "" : ";") f[i]; print s } n = 0; next }
            /^[^ \t]/ { next }
            { name_field = $2; sub(/\+0x[0-9a-f]+$/, "", name_field); f[++n] = name_field }
        ' | sort | uniq -c | sort -rn > "$OUT/$name.folded.txt"
    say "  $name: $(wc -l < "$OUT/$name.flat.txt") flat lines, $(wc -l < "$OUT/$name.folded.txt") unique stacks"
done

say "done; reports in $OUT"
head -40 "$OUT/sender.flat.txt" 2>/dev/null
