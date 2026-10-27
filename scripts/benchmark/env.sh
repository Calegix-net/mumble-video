# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# The one place this harness decides where anything is. Sourced by every script beside it.
#
# Every value can be overridden from the environment, because the three machines this has to run on -
# a developer's box, a WSL virtual machine and a CI runner - disagree about all of them. Nothing here
# is written outside MV_WORK, and MV_WORK defaults under the cache directory rather than into the
# source tree, so a benchmark never shows up in `git status`.

MV_SCRIPTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

#: The source tree. Two directories up from here, unless told otherwise.
MV_SRC="${MUMBLE_SRC:-$(cd "$MV_SCRIPTS/../.." && pwd)}"
#: Where the client and server were built.
MV_BUILD="${MUMBLE_BUILD:-$MV_SRC/build}"
#: Everything this harness creates: the rig, the runs, the tools it builds.
MV_WORK="${MUMBLE_BENCH_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/mumble-video-bench}"

MV_OUT="$MV_WORK/out"
MV_RIG="$MV_WORK/rig"
MV_TOOLS="$MV_WORK/tools"

MV_CLIENT="$MV_BUILD/mumble"
MV_SERVER="$MV_BUILD/mumble-server"

#: numpy is the only third-party requirement, for the summariser's percentiles. setup-rig.sh makes a
#: virtual environment with it; anything else with numpy on the path works too.
MV_PYTHON="${MUMBLE_BENCH_PYTHON:-$MV_WORK/venv/bin/python}"
#: The VMAF command-line tool. build-vmaf.sh puts one here; a distribution package works as well.
MV_VMAF="${MUMBLE_VMAF:-$MV_TOOLS/vmaf-install/bin/vmaf}"

# Fixed, and not an accident. A client stores its trust in the server's certificate against a
# hostname and port, so the pair the rig was set up for is the pair it has to connect to. Changing
# these means running setup-rig.sh again.
MV_HOST="${MUMBLE_BENCH_HOST:-127.0.0.1}"
MV_PORT="${MUMBLE_BENCH_PORT:-64740}"
MV_HOSTPORT="$MV_HOST:$MV_PORT"

# The two X displays the clients get. High numbers so that a real desktop's :0 is never touched.
MV_SENDER_DISPLAY="${MUMBLE_BENCH_SENDER_DISPLAY:-:90}"
MV_RECEIVER_DISPLAY="${MUMBLE_BENCH_RECEIVER_DISPLAY:-:91}"

# Exported rather than only assigned, because budget.py and compare.py read MV_OUT from the
# environment: it is how the shell and the Python agree on where runs live without the Python having to
# parse this file. A plain assignment is invisible to a child process, and the symptom of getting this
# wrong is budget.py reporting "no results" for a run sitting in front of it.
export MV_SCRIPTS MV_SRC MV_BUILD MV_WORK MV_OUT MV_RIG MV_TOOLS
export MV_CLIENT MV_SERVER MV_PYTHON MV_VMAF
export MV_HOST MV_PORT MV_HOSTPORT MV_SENDER_DISPLAY MV_RECEIVER_DISPLAY

mv_require() {
	local missing=0

	for tool in "$@"; do
		command -v "$tool" > /dev/null 2>&1 || {
			echo "missing: $tool" >&2
			missing=1
		}
	done

	return $missing
}

mv_check_build() {
	if [ ! -x "$MV_CLIENT" ] || [ ! -x "$MV_SERVER" ]; then
		echo "no client or server at $MV_BUILD" >&2
		echo "build them first, or point MUMBLE_BUILD at a build that has them" >&2
		return 1
	fi
}
