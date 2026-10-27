#!/usr/bin/env python3
"""Turns one benchmark run's logs into a table and a JSON file.

Every figure here comes from either the client's own MUMBLE_VIDEO_STATS lines or /proc, and only from
inside the measurement window the runner recorded - the connect, the first keyframe burst and the
encoder finding its rate are all outside it, and averaging them in would describe a moment nobody
watches.
"""

import csv
import datetime as dt
import json
import pathlib
import re
import sys

import numpy as np

# Two different numbering schemes reach these logs under the same field name "codec=", which is a trap
# worth naming here rather than tripping over twice. The tx line prints VideoBroadcaster's own constants
# and the rx line prints the wire enum from Mumble.proto; they disagree on every value above 1.
TX_CODECS = {0: "VP8", 1: "tiles", 2: "H.264", 3: "VP9"}
RX_CODECS = {0: "unknown", 1: "tiles", 2: "VP8", 3: "VP9", 4: "AV1", 5: "opus", 6: "H.264"}

# <I>2026-10-12 08:55:58.997 VideoStats tx stream=2 codec=2 fps=31.3 ...
LINE = re.compile(r"^<.>(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\s+(.*)$")
FIELD = re.compile(r"([A-Za-z0-9_/]+)=(-?[0-9.]+)")


def parse_log(path, window):
    """Every timestamped stats line in the file that falls inside the window, as dicts by kind."""
    out = {"tx": [], "rx": [], "gui": [], "nack": []}
    if not path.exists():
        return out

    start, end = window
    for raw in path.read_text(errors="replace").splitlines():
        m = LINE.match(raw)
        if not m:
            continue
        stamp = dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
        if not (start <= stamp <= end):
            continue

        body = m.group(2)
        if body.startswith("VideoStats tx"):
            kind = "tx"
        elif body.startswith("VideoStats rx"):
            kind = "rx"
        elif body.startswith("GuiLag"):
            kind = "gui"
        elif body.startswith("VideoNackStats"):
            kind = "nack"
        else:
            continue

        row = {k: float(v) for k, v in FIELD.findall(body)}
        row["_t"] = stamp
        out[kind].append(row)
    return out


def stat(rows, key, how="mean"):
    values = [r[key] for r in rows if key in r]
    if not values:
        return None
    a = np.array(values, dtype=float)
    return {
        "mean": float(a.mean()),
        "max": float(a.max()),
        "min": float(a.min()),
        "sum": float(a.sum()),
        "p50": float(np.percentile(a, 50)),
        "p90": float(np.percentile(a, 90)),
    }[how]


def resources(path, window):
    """Mean and peak CPU and RSS per process, from the runner's /proc samples."""
    out = {}
    if not path.exists():
        return out
    start, end = window
    rows = list(csv.DictReader(path.read_text().splitlines()))
    for name in sorted({r["proc"] for r in rows}):
        mine = [r for r in rows if r["proc"] == name and start <= float(r["epoch"]) <= end]
        if not mine:
            continue
        cpu = np.array([float(r["cpu_pct"]) for r in mine])
        rss = np.array([float(r["rss_kb"]) for r in mine])
        out[name] = {
            "cpu_pct_mean": round(float(cpu.mean()), 1),
            "cpu_pct_p90": round(float(np.percentile(cpu, 90)), 1),
            "cpu_pct_max": round(float(cpu.max()), 1),
            "rss_mb_mean": round(float(rss.mean()) / 1024.0, 1),
            "rss_mb_max": round(float(rss.max()) / 1024.0, 1),
            "rss_mb_growth": round(float(rss[-1] - rss[0]) / 1024.0, 1),
            "threads": int(float(mine[-1]["threads"])),
            "samples": len(mine),
        }
    return out


def gpu(path, window):
    if not path.exists():
        return {}
    start, end = window
    rows = list(csv.DictReader(path.read_text().splitlines()))
    mine = [r for r in rows if r.get("epoch") and start <= float(r["epoch"]) <= end]
    if not mine:
        return {}
    try:
        g = np.array([float(r["gpu_pct"]) for r in mine])
        e = np.array([float(r["encoder_pct"]) for r in mine])
    except (KeyError, ValueError):
        return {}
    return {
        "gpu_pct_mean": round(float(g.mean()), 1),
        "gpu_pct_max": round(float(g.max()), 1),
        "encoder_pct_mean": round(float(e.mean()), 1),
        "encoder_pct_max": round(float(e.max()), 1),
    }


def ping_line(path):
    if not path.exists():
        return ""
    for raw in path.read_text(errors="replace").splitlines():
        if "rtt min/avg/max" in raw:
            return raw.strip()
    return ""


def summarise(scenario_dir):
    try:
        start = float((scenario_dir / "window-start").read_text().strip())
        end = float((scenario_dir / "window-end").read_text().strip())
    except (OSError, ValueError):
        return None
    window = (start, end)

    sender = parse_log(scenario_dir / "sender.log", window)
    receiver = parse_log(scenario_dir / "receiver.log", window)

    # Only the picture stream. A screen share also announces its audio as a stream, and averaging a
    # codec=5 Opus line into the video figures would halve the frame rate on paper.
    # Only the picture stream on the rx side: a screen share also announces its audio as a stream, and
    # averaging a codec=5 Opus line into the video figures would halve the frame rate on paper. The tx
    # side needs no such filter - share audio has a broadcaster of its own that logs nothing here.
    tx = sender["tx"]
    rx = [r for r in receiver["rx"] if r.get("codec") != 5]

    # The quality numbers come from this scenario's own quality pass, which ran as a separate process
    # pair so that saving a PNG per frame could not be charged to the CPU figures above.
    quality = {}
    for qpath in (scenario_dir / "quality.json",
                  scenario_dir.parent / (scenario_dir.name + ".quality") / "quality.json"):
        if qpath.exists():
            quality = json.loads(qpath.read_text())
            break

    return {
        "scenario": scenario_dir.name,
        "window_seconds": round(end - start, 1),
        "ping": ping_line(scenario_dir / "run.log"),
        "codec": TX_CODECS.get(int(tx[0]["codec"]), "?") if tx else "?",
        "rx_codec": RX_CODECS.get(int(rx[0]["codec"]), "?") if rx else "?",
        "tx": {
            "fps": round(stat(tx, "fps") or 0, 1),
            "kbit_s": round(stat(tx, "kbit/s") or 0, 0),
            "encode_ms_avg": round(stat(tx, "encodeMsAvg") or 0, 2),
            "encode_ms_max": round(stat(tx, "encodeMsMax", "max") or 0, 2),
            "units_s": round(stat(tx, "units/s") or 0, 0),
            "max_units_per_frame": int(stat(tx, "maxUnitsPerFrame", "max") or 0),
            "dropped": int(stat(tx, "dropped", "sum") or 0),
            "keyframes": int(stat(tx, "keyframes", "sum") or 0),
            "threaded": bool(stat(tx, "threaded", "max")),
            "lines": len(tx),
        },
        "rx": {
            "frames_per_s": round((stat(rx, "frames", "sum") or 0) / max(1.0, end - start), 1),
            # Units and tiles separately from frames, so "nothing arrives" can be told apart from
            # "plenty arrives and no frame ever completes" - which is what a starved link looks like.
            "units_per_s": round((stat(rx, "units", "sum") or 0) / max(1.0, end - start), 1),
            "tiles_per_s": round((stat(rx, "tiles", "sum") or 0) / max(1.0, end - start), 1),
            # Arrival-to-paint, above the smallest arrival delay seen: decode, the playout buffer's
            # wait, and GUI-thread contention. Not the whole of glass-to-glass - the sender's
            # capture-to-send time is below the floor and invisible from the receiver.
            "paint_delay_ms_p50": round(stat(rx, "paintDelayMsP50", "p50") or -1, 1),
            "paint_delay_ms_p90": round(stat(rx, "paintDelayMsP90", "p90") or -1, 1),
            "paint_delay_ms_max": round(stat(rx, "paintDelayMsMax", "max") or -1, 1),
            "paint_delay_samples": int(stat(rx, "paintDelaySamples", "sum") or 0),
            "max_paint_gap_ms": int(stat(rx, "maxGapMs", "max") or 0),
            "keyframe_req_loss": int(stat(rx, "kfLoss", "sum") or 0),
            "keyframe_req_overload": int(stat(rx, "kfOverload", "sum") or 0),
            "gaps_recovered": int(stat(rx, "gapsRecovered", "max") or 0),
            "gaps_abandoned": int(stat(rx, "gapsAbandoned", "max") or 0),
            "lines": len(rx),
        },
        "gui_lag": {
            "worst_ms": int(stat(receiver["gui"], "worstMs", "max") or 0),
            "ticks_over_16ms": int(stat(receiver["gui"], "ticksOver16ms", "sum") or 0),
            "sender_worst_ms": int(stat(sender["gui"], "worstMs", "max") or 0),
        },
        # The two halves of VideoNackStats are counted in different processes and the same log line
        # prints both, so a receiver's copy always reads zero for the answering fields and a sender's
        # always reads zero for the asking ones. requestsAnswered/fragmentsResent/unitsNoLongerHeld are
        # incremented in ServerHandler::resendVideoUnits(), which runs on the sender.
        "nack": {
            "requests_sent": int(stat(receiver["nack"], "requestsSent", "sum") or 0),
            "units_requested": int(stat(receiver["nack"], "unitsRequested", "sum") or 0),
            "whole_frames_requested": int(stat(receiver["nack"], "wholeFramesRequested", "sum") or 0),
            "requests_answered": int(stat(sender["nack"], "requestsAnswered", "sum") or 0),
            "fragments_resent": int(stat(sender["nack"], "fragmentsResent", "sum") or 0),
            "units_no_longer_held": int(stat(sender["nack"], "unitsNoLongerHeld", "sum") or 0),
            "resend_budget_hit": int(stat(sender["nack"], "resendBudgetHit", "sum") or 0),
        },
        "resources": resources(scenario_dir / "samples.csv", window),
        "gpu": gpu(scenario_dir / "gpu.csv", window),
        "quality": quality,
    }


def table(results):
    rows = [
        ("scenario", lambda r: r["scenario"]),
        ("codec", lambda r: r["codec"]),
        ("tx fps", lambda r: f"{r['tx']['fps']:.1f}"),
        ("tx Mbit/s", lambda r: f"{r['tx']['kbit_s'] / 1000:.2f}"),
        ("encode ms avg", lambda r: f"{r['tx']['encode_ms_avg']:.2f}"),
        ("encode ms max", lambda r: f"{r['tx']['encode_ms_max']:.2f}"),
        ("tx dropped", lambda r: str(r["tx"]["dropped"])),
        ("rx units/s", lambda r: f"{r['rx']['units_per_s']:.0f}"),
        ("rx frames/s", lambda r: f"{r['rx']['frames_per_s']:.1f}"),
        ("paint delay p50 ms", lambda r: f"{r['rx']['paint_delay_ms_p50']:.0f}"),
        ("paint delay p90 ms", lambda r: f"{r['rx']['paint_delay_ms_p90']:.0f}"),
        ("paint delay max ms", lambda r: f"{r['rx']['paint_delay_ms_max']:.0f}"),
        ("max paint gap ms", lambda r: str(r["rx"]["max_paint_gap_ms"])),
        ("keyframe reqs (loss)", lambda r: str(r["rx"]["keyframe_req_loss"])),
        ("gaps abandoned", lambda r: str(r["rx"]["gaps_abandoned"])),
        ("nack units asked", lambda r: str(r["nack"]["units_requested"])),
        ("nack answered (sender)", lambda r: str(r["nack"]["requests_answered"])),
        ("nack resent (sender)", lambda r: str(r["nack"]["fragments_resent"])),
        ("nack too old (sender)", lambda r: str(r["nack"]["units_no_longer_held"])),
        ("sender CPU %", lambda r: f"{r['resources'].get('sender', {}).get('cpu_pct_mean', 0):.0f}"),
        ("receiver CPU %", lambda r: f"{r['resources'].get('receiver', {}).get('cpu_pct_mean', 0):.0f}"),
        ("server CPU %", lambda r: f"{r['resources'].get('server', {}).get('cpu_pct_mean', 0):.0f}"),
        ("sender RSS MB", lambda r: f"{r['resources'].get('sender', {}).get('rss_mb_mean', 0):.0f}"),
        ("receiver RSS MB", lambda r: f"{r['resources'].get('receiver', {}).get('rss_mb_mean', 0):.0f}"),
        ("GUI lag worst ms", lambda r: str(r["gui_lag"]["worst_ms"])),
        ("GPU %", lambda r: f"{r['gpu'].get('gpu_pct_mean', 0):.0f}"),
        ("NVENC %", lambda r: f"{r['gpu'].get('encoder_pct_mean', 0):.0f}"),
        ("VMAF", lambda r: f"{r['quality']['vmaf']:.1f}"),
        ("VMAF min", lambda r: f"{r['quality']['vmaf_min']:.1f}"),
        ("PSNR-Y dB", lambda r: f"{r['quality']['psnr_y_db']:.1f}"),
        ("SSIM", lambda r: f"{r['quality']['ssim']:.4f}"),
        ("frames scored", lambda r: str(r['quality']['frames_scored'])),
    ]

    width = max(len(label) for label, _ in rows) + 2
    lines = []
    header = "metric".ljust(width) + "".join(r["scenario"].rjust(14) for r in results)
    lines.append(header)
    lines.append("-" * len(header))
    for label, get in rows[1:]:
        cells = []
        for r in results:
            try:
                cells.append(get(r).rjust(14))
            except Exception:
                cells.append("-".rjust(14))
        lines.append(label.ljust(width) + "".join(cells))
    return "\n".join(lines)


def main():
    run = pathlib.Path(sys.argv[1])
    results = []
    # The .quality directories are read through their base scenario, not reported beside it.
    for d in sorted(p for p in run.iterdir() if p.is_dir() and not p.name.endswith(".quality")):
        r = summarise(d)
        if r:
            results.append(r)

    if not results:
        print("no completed scenarios in", run)
        return 1

    (run / "results.json").write_text(json.dumps(results, indent=2))
    print(table(results))
    print()
    for r in results:
        print(f"{r['scenario']}: {r['ping']}  (window {r['window_seconds']}s, "
              f"{r['tx']['lines']} tx lines, {r['rx']['lines']} rx lines)")
    print()
    print("results.json:", run / "results.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
