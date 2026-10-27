#!/bin/bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# One benchmark run, from inside an unshared network namespace.
#
# Everything lives in that namespace so that tc can shape loopback without root: the namespace is owned
# by a user namespace in which we are uid 0, which is enough for CAP_NET_ADMIN over its own interfaces
# and nothing else on the machine. The server, both clients and both X servers are started in here, so
# all of their traffic crosses the shaped loopback.
#
#   inner-bench.sh <scenario> <seconds> <outdir> <codec: auto|vp9> <mode: perf|quality>
#
# perf and quality are separate runs of the same scenario on purpose. Saving a PNG per frame costs real
# CPU and real disk, so a run that measures CPU must not be saving frames, and a run that saves frames
# cannot be trusted for CPU. The two are short enough that doing both is cheaper than arguing about how
# much the dumping cost.
set -u
# shellcheck source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

SCENARIO="$1"
WINDOW_SECONDS="$2"
OUT="$3"
CODEC="${4:-auto}"
MODE="${5:-perf}"


mkdir -p "$OUT"
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/run.log"; }

# ---------------------------------------------------------------------------------------------------
# The impairments. A netem delay is one-way per direction, so a round trip sees it twice; the ping
# below records what actually happened rather than what was asked for.
# ---------------------------------------------------------------------------------------------------
netem_for() {
    case "$1" in
        clean)    echo "" ;;
        loss2)    echo "netem loss 2%" ;;
        delay100) echo "netem delay 100ms" ;;
        bw2mbit)  echo "netem rate 2mbit" ;;
        bad)      echo "netem delay 100ms loss 2% rate 2mbit" ;;
        # The link starts clean and is taken away mid-window instead. See DROP_* below.
        drop)     echo "" ;;
        *)        echo "UNKNOWN" ;;
    esac
}

ip link set lo up
QDISC="$(netem_for "$SCENARIO")"
if [ "$QDISC" = "UNKNOWN" ]; then
    say "unknown scenario $SCENARIO"
    exit 2
fi

tc qdisc del dev lo root 2>/dev/null
if [ -n "$QDISC" ]; then
    # shellcheck disable=SC2086
    tc qdisc add dev lo root $QDISC || { say "tc failed"; exit 2; }
fi
say "scenario=$SCENARIO codec=$CODEC mode=$MODE qdisc=${QDISC:-none}"
tc qdisc show dev lo >> "$OUT/run.log"
ping -c 5 -i 0.3 -q 127.0.0.1 2>&1 | tail -2 | tee -a "$OUT/run.log"

# The codec is a setting rather than an environment variable: screenShareCodec() reads
# video.screen_share_codec, where 1 means "VP9 even if a hardware H.264 encoder would open".
"$MV_PYTHON" - "$MV_WORK/clients/sender/.config/Mumble/Mumble/mumble_settings.json" "$CODEC" <<'PY'
import json, sys
path, codec = sys.argv[1], sys.argv[2]
with open(path) as f:
    s = json.load(f)
s.setdefault("video", {})["screen_share_codec"] = 1 if codec == "vp9" else 0
with open(path, "w") as f:
    json.dump(s, f, indent=2)
PY

# ---------------------------------------------------------------------------------------------------
# Processes, started in dependency order and torn down in reverse by cleanup().
# ---------------------------------------------------------------------------------------------------
PIDS=()
cleanup() {
    say "tearing down"
    for d in "$MV_WORK/clients/sender" "$MV_WORK/clients/receiver"; do
        HOME="$d" XDG_RUNTIME_DIR="$d/run" timeout 10 "$MV_CLIENT" rpc quit >/dev/null 2>&1
    done
    sleep 2
    for p in ${PIDS[@]+"${PIDS[@]}"}; do kill "$p" 2>/dev/null; done
    sleep 1
    for p in ${PIDS[@]+"${PIDS[@]}"}; do kill -9 "$p" 2>/dev/null; done
    tc qdisc del dev lo root 2>/dev/null
}
trap cleanup EXIT

Xvfb "$MV_SENDER_DISPLAY" -screen 0 1920x1080x24 -extension GLX > "$OUT/xvfb-sender.log" 2>&1 &
PIDS+=($!)
Xvfb "$MV_RECEIVER_DISPLAY" -screen 0 1920x1080x24 -extension GLX > "$OUT/xvfb-receiver.log" 2>&1 &
PIDS+=($!)
sleep 2

say "starting server"
"$MV_SERVER" --foreground --ini "$MV_WORK/server/mumble-server.ini" > "$OUT/server.stdout" 2>&1 &
SERVER_PID=$!
PIDS+=("$SERVER_PID")
sleep 3
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    say "server died; last lines:"
    tail -20 "$OUT/server.stdout" | tee -a "$OUT/run.log"
    exit 3
fi
say "server pid $SERVER_PID"

start_client() {
    local name="$1" display="$2" user="$3"
    shift 3
    local dir="$MV_WORK/clients/$name"
    mkdir -p "$dir/run"
    chmod 700 "$dir/run"
    rm -f "$dir/run/MumbleSocket" "$dir/mumble.lock"

    env HOME="$dir" \
        XDG_RUNTIME_DIR="$dir/run" \
        XDG_CONFIG_HOME="$dir/.config" \
        XDG_DATA_HOME="$dir/.local/share" \
        DISPLAY="$display" \
        QT_QPA_PLATFORM=xcb \
        MUMBLE_VIDEO_STATS=1 \
        "$@" \
        "$MV_CLIENT" --skip-settings-backup-prompt "mumble://$user@$MV_HOSTPORT/" \
        > "$OUT/$name.log" 2>&1 &
    echo $!
}

# The sender streams the deterministic mock desktop: a static wallpaper, a terminal whose text scrolls
# one line every four frames, a block that moves every frame, and a frame counter. That mix - mostly
# unchanged pixels with a few regions always changing - is what a shared desktop actually looks like,
# and the scrolling text is the content a screen-share codec is judged on.
SENDER_ENV=(env MUMBLE_MOCK_SCREEN=1 MUMBLE_MOCK_SCREEN_SIZE=1920x1080)
RECEIVER_ENV=(env)

if [ "$MODE" = "quality" ]; then
    mkdir -p "$OUT/dump-tx" "$OUT/dump-rx"
    SENDER_ENV+=("MUMBLE_VIDEO_DUMP_TX=$OUT/dump-tx")
    RECEIVER_ENV+=("MUMBLE_VIDEO_DUMP_RX=$OUT/dump-rx")
fi

SENDER_PID=$(start_client sender "$MV_SENDER_DISPLAY" qa-mv1 "${SENDER_ENV[@]}")
PIDS+=("$SENDER_PID")
RECEIVER_PID=$(start_client receiver "$MV_RECEIVER_DISPLAY" qa-mv2 "${RECEIVER_ENV[@]}")
PIDS+=("$RECEIVER_PID")
say "sender pid $SENDER_PID, receiver pid $RECEIVER_PID"
sleep 8

rpc() {
    local dir="$MV_WORK/clients/$1"
    shift
    HOME="$dir" XDG_RUNTIME_DIR="$dir/run" timeout 10 "$MV_CLIENT" rpc "$@" >/dev/null 2>&1
}

# Asked for repeatedly rather than once: a client that has not finished connecting accepts the request
# and does nothing useful with it, and no log line reliably says "connected". The stats lines are the
# real signal - they appear only once frames are moving - so they are what is waited for.
say "asking the sender to share, until it does"
for _ in $(seq 1 30); do
    rpc sender sharescreen
    sleep 2
    grep -q "VideoStats tx" "$OUT/sender.log" && break
done
if ! grep -q "VideoStats tx" "$OUT/sender.log"; then
    say "sender never started encoding; last lines:"
    tail -25 "$OUT/sender.log" | tee -a "$OUT/run.log"
    exit 4
fi
grep -m1 "Screen share:" "$OUT/sender.log" | tee -a "$OUT/run.log"

say "asking the receiver to watch, until it does"
for _ in $(seq 1 30); do
    rpc receiver watchall
    sleep 2
    grep -q "VideoStats rx" "$OUT/receiver.log" && break
done
if ! grep -q "VideoStats rx" "$OUT/receiver.log"; then
    say "receiver never painted a frame; last lines:"
    tail -25 "$OUT/receiver.log" | tee -a "$OUT/run.log"
    exit 5
fi
say "receiver is watching"

# ---------------------------------------------------------------------------------------------------
# The measurement window. Everything before it - connecting, the first keyframe burst, the encoder
# finding its rate - is start-up, and averaging that in would describe a moment nobody watches.
# ---------------------------------------------------------------------------------------------------
say "settling for 10s, then measuring for ${WINDOW_SECONDS}s"
sleep 10
date +%s > "$OUT/window-start"
echo "epoch,proc,cpu_pct,rss_kb,threads" > "$OUT/samples.csv"
echo "epoch,gpu_pct,encoder_pct,mem_used_mib" > "$OUT/gpu.csv"

declare -A PREV_TICKS
HZ=$(getconf CLK_TCK)

# CPU per process from /proc, as a percentage of one core over the sample interval. The process name in
# /proc/pid/stat can contain spaces and brackets, so fields are counted from after the closing bracket.
sample_proc() {
    local label="$1" pid="$2" now="$3"
    [ -d "/proc/$pid" ] || return
    local rest ticks rss threads prev
    rest=$(sed 's/^.*) //' "/proc/$pid/stat" 2>/dev/null) || return
    ticks=$(awk '{print $12 + $13}' <<< "$rest")
    rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)
    threads=$(awk '/Threads/{print $2}' "/proc/$pid/status" 2>/dev/null)
    prev="${PREV_TICKS[$label]:-}"
    PREV_TICKS[$label]="$ticks"
    [ -z "$prev" ] && return
    awk -v e="$now" -v l="$label" -v d="$((ticks - prev))" -v hz="$HZ" -v r="${rss:-0}" -v t="${threads:-0}" \
        'BEGIN { printf "%s,%s,%.1f,%s,%s\n", e, l, 100.0 * d / hz, r, t }' >> "$OUT/samples.csv"
}

# The `drop` scenario: the link disappears entirely, part way through, and comes back. Written as
# 100 % loss rather than by taking the interface down, because a down interface fails a send
# immediately with an error the socket layer reports, where a black hole does not - and a black hole
# is what a real network does when a switch reboots or a laptop changes access point.
#
# What it is asking: the receiver has to notice it is receiving nothing, ask for a keyframe, keep
# asking while the asking cannot get through, and show a picture again afterwards. A pipeline can be
# well behaved under a steady 2 % loss and still never recover from this, because the one request it
# makes is lost along with everything else.
DROP_AT=$(( WINDOW_SECONDS / 3 ))
DROP_FOR=5
DROPPED=0
RESTORED=0

END=$(( $(date +%s) + WINDOW_SECONDS ))
STARTED=$(date +%s)
while [ "$(date +%s)" -lt "$END" ]; do
    NOW=$(date +%s)

    if [ "$SCENARIO" = "drop" ]; then
        ELAPSED=$(( NOW - STARTED ))

        if [ "$DROPPED" = "0" ] && [ "$ELAPSED" -ge "$DROP_AT" ]; then
            tc qdisc add dev lo root netem loss 100% 2>/dev/null && DROPPED=1
            say "link dropped at +${ELAPSED}s"
            echo "$NOW" > "$OUT/drop-start"
        elif [ "$DROPPED" = "1" ] && [ "$RESTORED" = "0" ] && [ "$ELAPSED" -ge $(( DROP_AT + DROP_FOR )) ]; then
            tc qdisc del dev lo root 2>/dev/null && RESTORED=1
            say "link restored at +${ELAPSED}s"
            echo "$NOW" > "$OUT/drop-end"
        fi
    fi

    sample_proc server "$SERVER_PID" "$NOW"
    sample_proc sender "$SENDER_PID" "$NOW"
    sample_proc receiver "$RECEIVER_PID" "$NOW"
    nvidia-smi --query-gpu=utilization.gpu,utilization.encoder,memory.used \
        --format=csv,noheader,nounits 2>/dev/null \
        | head -1 | sed "s/^/$NOW,/; s/, */,/g" >> "$OUT/gpu.csv"
    sleep 1
done
date +%s > "$OUT/window-end"
say "window closed"

# The clients are stopped before scoring, so that no frame is half written when it is read.
if [ "$MODE" = "quality" ]; then
    cleanup
    trap - EXIT
    say "scoring picture quality"
    bash "$MV_SCRIPTS/quality.sh" "$OUT" 2>&1 | tee -a "$OUT/run.log"
    # Several hundred 1080p PNGs per side. The scores and the list of frame numbers behind them are
    # kept; the pictures themselves are not worth the gigabytes.
    rm -rf "$OUT/dump-tx" "$OUT/dump-rx" "$OUT/quality"/*.png "$OUT/quality"/*.y4m
fi

say "run finished"
