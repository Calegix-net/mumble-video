#!/bin/bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# Builds libvmaf's own CLI, because the distribution ffmpeg has no libvmaf filter and rebuilding ffmpeg
# to get one would be a far larger job than building the tool it would have called. The CLI takes two
# y4m files, which is exactly what ffmpeg can make out of a sequence of PNGs.
#
# Skip this entirely if the distribution has a `vmaf` binary: point MUMBLE_VMAF at it instead. This is
# here because Ubuntu does not.
set -u
# shellcheck source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

VMAF_SRC="$MV_TOOLS/vmaf"
VMAF_PREFIX="$MV_TOOLS/vmaf-install"
LOGS="$MV_TOOLS/logs"

mv_require git ninja || exit 1
mkdir -p "$MV_TOOLS" "$LOGS"

MESON="$MV_WORK/venv/bin/meson"
if [ ! -x "$MESON" ]; then
	MESON=$(command -v meson) || {
		echo "no meson; run setup-rig.sh first, or install meson" >&2
		exit 1
	}
fi

if [ ! -d "$VMAF_SRC/.git" ]; then
	git clone --depth 1 https://github.com/Netflix/vmaf.git "$VMAF_SRC" || exit 1
fi

cd "$VMAF_SRC/libvmaf" || exit 1
rm -rf build

# -Denable_asm=false because the hand-written assembly wants an nasm newer than the distribution's, and
# the scores are identical either way - only the speed differs, and this scores a few hundred frames.
"$MESON" setup build --buildtype release -Denable_asm=false --prefix "$VMAF_PREFIX" \
	> "$LOGS/vmaf-setup.log" 2>&1 || {
	tail -20 "$LOGS/vmaf-setup.log"
	exit 1
}

ninja -C build > "$LOGS/vmaf-build.log" 2>&1 || {
	tail -30 "$LOGS/vmaf-build.log"
	exit 1
}
ninja -C build install > "$LOGS/vmaf-install.log" 2>&1 || {
	tail -20 "$LOGS/vmaf-install.log"
	exit 1
}

echo "vmaf CLI: $VMAF_PREFIX/bin/vmaf"
"$VMAF_PREFIX/bin/vmaf" --version 2>&1 | head -2
