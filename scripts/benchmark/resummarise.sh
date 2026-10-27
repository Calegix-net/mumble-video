#!/bin/bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# Re-derives summary.txt and results.json for runs already on disk. The summariser reads the raw logs,
# so a fix to what it extracts - such as reading the NACK answering counters from the sender's log
# rather than the receiver's, where they are always zero - applies retrospectively without re-running
# a single benchmark.
set -u
# shellcheck source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
for label in "$@"; do
    if [ -d "$MV_OUT/$label" ]; then
        "$MV_PYTHON" "$MV_SCRIPTS/summarise.py" "$MV_OUT/$label" > "$MV_OUT/$label/summary.txt" 2>&1
        echo "$label: $?"
    else
        echo "$label: no such run"
    fi
done
