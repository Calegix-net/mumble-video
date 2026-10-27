#!/bin/bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# Picture quality for one scenario: PSNR, SSIM and VMAF between the frames the sender encoded and the
# frames the receiver decoded.
#
# The pairing is exact rather than approximate. Both sides name their files with the sender's frame
# number, which travels on every unit, so tx-<s>-<n>.png and rx-<s>-<n>.png are one frame before and
# after the pipeline. Only numbers present on both sides are compared - a frame the receiver never got
# is a loss, which the rx stats already count, and feeding it to a quality metric as a black frame
# would report the loss twice and call it blur.
#
# Usage: quality.sh <scenario-dir>
set -u
# shellcheck source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
DIR="$1"
TX="$DIR/dump-tx"
RX="$DIR/dump-rx"
WORK="$DIR/quality"

rm -rf "$WORK"
mkdir -p "$WORK"

if [ ! -d "$TX" ] || [ ! -d "$RX" ]; then
    echo '{"error": "no frame dumps"}' > "$DIR/quality.json"
    echo "no frame dumps in $DIR"
    exit 0
fi

# Matched frame numbers, newest first: the oldest frames in the dump are from the settling period
# before the measurement window.
"$MV_PYTHON" - "$TX" "$RX" "$WORK" <<'PY'
import pathlib, re, sys

tx_dir, rx_dir, work = (pathlib.Path(p) for p in sys.argv[1:4])
NAME = re.compile(r"^(tx|rx)-(\d+)-(\d+)\.png$")


def index(d, kind):
    out = {}
    for p in d.iterdir():
        m = NAME.match(p.name)
        if m and m.group(1) == kind:
            out[int(m.group(3))] = p
    return out


def usable(path):
    """Whether the file is a complete PNG - it ends with an IEND chunk rather than stopping short."""
    try:
        with open(path, "rb") as f:
            f.seek(-12, 2)
            return f.read()[4:8] == b"IEND"
    except OSError:
        return False


tx, rx = index(tx_dir, "tx"), index(rx_dir, "rx")
both = sorted(set(tx) & set(rx))

# The newest few are dropped unread. The clients are stopped before this runs, but the last file each
# side wrote can still be the one it was part way through, and a truncated PNG decodes to garbage that
# a quality metric cheerfully scores as a terrible frame - which is how a torn file becomes a false
# "worst frame" in the report.
if len(both) > 6:
    both = both[:-6]

# Then the last 120, which is four seconds at 30fps: enough for a stable score, few enough that
# building the y4m pair and scoring it stays quick.
chosen = [n for n in both[-120:] if usable(tx[n]) and usable(rx[n])]
(work / "pairs.txt").write_text("\n".join(str(n) for n in chosen))
print("tx frames:", len(tx), " rx frames:", len(rx), " usable matched:", len(both),
      " scored:", len(chosen))

for i, n in enumerate(chosen):
    (work / f"ref-{i:05d}.png").symlink_to(tx[n].resolve())
    (work / f"dis-{i:05d}.png").symlink_to(rx[n].resolve())
PY

COUNT=$(ls "$WORK"/ref-*.png 2>/dev/null | wc -l)
if [ "$COUNT" -lt 10 ]; then
    echo "{\"error\": \"only $COUNT matched frames\"}" > "$DIR/quality.json"
    echo "only $COUNT matched frames; not scoring"
    exit 0
fi

# y4m, because that is what the vmaf CLI reads, and it is lossless so it adds nothing to the scores.
# The reference's size is the geometry both are coerced to: a decoder can hand back a differently sized
# picture after a resize, and comparing two different sizes is not a comparison.
GEOM=$(identify -format "%wx%h" "$WORK/ref-00000.png")
ffmpeg -nostdin -hide_banner -loglevel error -framerate 30 -i "$WORK/ref-%05d.png" \
    -vf "scale=$GEOM,format=yuv420p" -y "$WORK/ref.y4m" || exit 1
ffmpeg -nostdin -hide_banner -loglevel error -framerate 30 -i "$WORK/dis-%05d.png" \
    -vf "scale=$GEOM,format=yuv420p" -y "$WORK/dis.y4m" || exit 1

WIDTH=${GEOM%x*}
HEIGHT=${GEOM#*x}

"$MV_VMAF" --reference "$WORK/ref.y4m" --distorted "$WORK/dis.y4m" \
    --model version=vmaf_v0.6.1 --feature psnr --feature float_ssim \
    --output "$WORK/vmaf.json" --json > "$WORK/vmaf.log" 2>&1
VMAF_EXIT=$?

"$MV_PYTHON" - "$WORK" "$DIR" "$WIDTH" "$HEIGHT" "$VMAF_EXIT" <<'PY'
import json, pathlib, sys

work, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
width, height, exit_code = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])

result = {"frames_scored": len(list(work.glob("ref-*.png"))), "geometry": f"{width}x{height}"}

path = work / "vmaf.json"
if exit_code != 0 or not path.exists():
    result["error"] = "vmaf failed; see " + str(work / "vmaf.log")
else:
    pooled = json.loads(path.read_text())["pooled_metrics"]
    for name, key in (("vmaf", "vmaf"), ("psnr_y_db", "psnr_y"), ("ssim", "float_ssim")):
        if key in pooled:
            result[name] = round(pooled[key]["mean"], 4)
            result[name + "_min"] = round(pooled[key]["min"], 4)

(out / "quality.json").write_text(json.dumps(result, indent=2))
print(json.dumps(result, indent=2))
PY
