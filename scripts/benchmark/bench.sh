#!/bin/bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# Runs a set of benchmark scenarios and summarises them.
#
#   bench.sh <run-label> <seconds> <codec: auto|vp9> <scenario>...
#
# Each scenario is run twice: once measuring the machine, once saving frames to score the picture (see
# inner-bench.sh for why those cannot be the same run). Every run gets its own network namespace, its
# own server and its own pair of clients, so one scenario's impairment, sockets and encoder state
# cannot reach the next.
set -u
# shellcheck source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

LABEL="$1"
WINDOW="$2"
CODEC="$3"
shift 3
SCENARIOS=("$@")

RUN="$MV_WORK/out/$LABEL"
mkdir -p "$RUN"

{
    echo "label:    $LABEL"
    echo "codec:    $CODEC"
    echo "window:   ${WINDOW}s per scenario"
    echo "built:    $(cd $MV_SRC && git log --oneline -1)"
    echo "client:   $($MV_CLIENT --version 2>&1 | head -1)"
    echo "host:     $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs), $(nproc) threads"
    echo "kernel:   $(uname -r)"
    echo "gpu:      $(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)"
    echo "rmem_max: $(cat /proc/sys/net/core/rmem_max)"
    echo "started:  $(date -Is)"
} | tee "$RUN/environment.txt"

for scenario in "${SCENARIOS[@]}"; do
    for mode in perf quality; do
        name="$scenario"
        [ "$mode" = "quality" ] && name="$scenario.quality"
        seconds="$WINDOW"
        # The quality pass only needs enough frames to score; it measures nothing about the machine.
        [ "$mode" = "quality" ] && seconds=14

        echo
        echo "================ $name ================"
        OUT="$RUN/$name"
        rm -rf "$OUT"
        # -r maps us to uid 0 in a new user namespace, which is what grants CAP_NET_ADMIN over the new
        # network namespace's own interfaces. Nothing outside it is affected.
        unshare -r --net "$MV_SCRIPTS/inner-bench.sh" "$scenario" "$seconds" "$OUT" "$CODEC" "$mode"
        echo "inner exit $?"
    done
done

echo
echo "================ summary ================"
"$MV_PYTHON" "$MV_SCRIPTS/summarise.py" "$RUN" | tee "$RUN/summary.txt"
