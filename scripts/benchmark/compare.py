#!/usr/bin/env python3
"""Puts two benchmark runs side by side, as text and as HTML table rows.

Usage: compare.py <before-label> <after-label> [--html]

Reads ~/mv-bench/out/<label>/results.json. A metric is only marked better or worse when it moved by
more than NOISE of its own value *and* by more than its own absolute floor - this rig's CPU and timing
figures wander by a few per cent between identical runs, and marking that as a result would be worse
than saying nothing.
"""

import json
import pathlib
import sys

# Its own directory, explicitly: python -I and python -P both omit it, and a benchmark script that
# fails to import its neighbour depending on how it was invoked is a bad surprise in a CI log.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import benchpaths  # noqa: E402  (after the path insert, necessarily)

OUT = benchpaths.output_directory()

#: Relative and absolute movement a metric must exceed before it is called a change.
NOISE = 0.05

# label, json path, lower-is-better (None = neither), format, scale, absolute floor
METRICS = [
    ("tx fps", ("tx", "fps"), False, "{:.1f}", 1.0, 0.5),
    ("tx Mbit/s", ("tx", "kbit_s"), None, "{:.2f}", 0.001, 0.05),
    ("encode ms avg", ("tx", "encode_ms_avg"), True, "{:.2f}", 1.0, 0.5),
    ("encode ms max", ("tx", "encode_ms_max"), True, "{:.2f}", 1.0, 2.0),
    ("tx dropped", ("tx", "dropped"), True, "{:.0f}", 1.0, 0.5),
    ("rx units/s", ("rx", "units_per_s"), None, "{:.0f}", 1.0, 1.0),
    ("rx frames/s", ("rx", "frames_per_s"), False, "{:.1f}", 1.0, 0.5),
    ("paint delay p50 ms", ("rx", "paint_delay_ms_p50"), True, "{:.0f}", 1.0, 5.0),
    ("paint delay p90 ms", ("rx", "paint_delay_ms_p90"), True, "{:.0f}", 1.0, 5.0),
    ("paint delay max ms", ("rx", "paint_delay_ms_max"), True, "{:.0f}", 1.0, 10.0),
    ("max paint gap ms", ("rx", "max_paint_gap_ms"), True, "{:.0f}", 1.0, 10.0),
    ("keyframe reqs (loss)", ("rx", "keyframe_req_loss"), True, "{:.0f}", 1.0, 0.5),
    ("gaps recovered", ("rx", "gaps_recovered"), False, "{:.0f}", 1.0, 5.0),
    ("gaps abandoned", ("rx", "gaps_abandoned"), True, "{:.0f}", 1.0, 0.5),
    ("nack units asked", ("nack", "units_requested"), None, "{:.0f}", 1.0, 5.0),
    ("nack answered", ("nack", "requests_answered"), False, "{:.0f}", 1.0, 5.0),
    ("nack resent", ("nack", "fragments_resent"), False, "{:.0f}", 1.0, 5.0),
    ("nack too old", ("nack", "units_no_longer_held"), True, "{:.0f}", 1.0, 5.0),
    ("sender CPU %", ("resources", "sender", "cpu_pct_mean"), True, "{:.0f}", 1.0, 2.0),
    ("receiver CPU %", ("resources", "receiver", "cpu_pct_mean"), True, "{:.0f}", 1.0, 2.0),
    ("server CPU %", ("resources", "server", "cpu_pct_mean"), True, "{:.0f}", 1.0, 1.0),
    ("sender RSS MB", ("resources", "sender", "rss_mb_mean"), True, "{:.0f}", 1.0, 15.0),
    ("receiver RSS MB", ("resources", "receiver", "rss_mb_mean"), True, "{:.0f}", 1.0, 15.0),
    ("GUI lag worst ms", ("gui_lag", "worst_ms"), True, "{:.0f}", 1.0, 3.0),
    ("NVENC %", ("gpu", "encoder_pct_mean"), None, "{:.0f}", 1.0, 2.0),
    ("VMAF", ("quality", "vmaf"), False, "{:.1f}", 1.0, 1.0),
    ("VMAF min", ("quality", "vmaf_min"), False, "{:.1f}", 1.0, 5.0),
    ("PSNR-Y dB", ("quality", "psnr_y_db"), False, "{:.1f}", 1.0, 1.0),
    ("SSIM", ("quality", "ssim"), False, "{:.4f}", 1.0, 0.001),
]


def dig(row, path):
    node = row
    for key in path:
        if not isinstance(node, dict) or key not in node:
            return None
        node = node[key]
    return node if isinstance(node, (int, float)) else None


def load(label):
    path = OUT / label / "results.json"
    if not path.exists():
        return None
    return {r["scenario"]: r for r in json.loads(path.read_text())}


def verdict(before, after, lower_better, floor):
    """"better", "worse" or None - None meaning the move is inside this rig's noise."""
    if lower_better is None or before is None or after is None:
        return None
    moved = abs(after - before)
    if moved <= floor or moved <= NOISE * abs(before):
        return None
    return "better" if ((after < before) == lower_better) else "worse"


def main():
    before_label, after_label = sys.argv[1], sys.argv[2]
    as_html = "--html" in sys.argv

    before, after = load(before_label), load(after_label)
    if before is None or after is None:
        print("missing results.json for", before_label if before is None else after_label)
        return 1

    scenarios = [
        s for s in ("clean", "loss2", "delay100", "bw2mbit", "bad", "drop") if s in before and s in after
    ]
    lines = []

    for name, path, lower_better, fmt, scale, floor in METRICS:
        cells = []
        for scenario in scenarios:
            b, a = dig(before[scenario], path), dig(after[scenario], path)
            bs = b * scale if b is not None else None
            as_ = a * scale if a is not None else None
            how = verdict(bs, as_, lower_better, floor)

            if as_html:
                bt = fmt.format(bs) if bs is not None else "&mdash;"
                at = fmt.format(as_) if as_ is not None else "&mdash;"
                cls = ' class="%s"' % how if how else ""
                cells.append("<td>%s</td><td%s>%s</td>" % (bt, cls, at))
            else:
                bt = fmt.format(bs) if bs is not None else "-"
                at = fmt.format(as_) if as_ is not None else "-"
                mark = {"better": "+", "worse": "!", None: " "}[how]
                cells.append(("%10s %10s %s" % (bt, at, mark)).center(24))

        if as_html:
            lines.append("<tr><th>%s</th>%s</tr>" % (name, "".join(cells)))
        else:
            lines.append(name.ljust(22) + "".join(cells))

    if as_html:
        print("\n".join(lines))
        return 0

    header = "metric".ljust(22) + "".join(s.center(24) for s in scenarios)
    print(header)
    print("".ljust(22) + "".join("    before      after   ".center(24) for _ in scenarios))
    print("-" * len(header))
    print("\n".join(lines))
    print()
    print("+ better, ! worse, blank = inside this rig's run-to-run noise")
    return 0


if __name__ == "__main__":
    sys.exit(main())
