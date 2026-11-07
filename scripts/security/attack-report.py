#!/usr/bin/env python3
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

"""Reads the JSONL that mv-attack emits and turns it into pass/fail for each scenario.

Two subcommands:
  session <file>   print the own_session of the client that wrote <file>, as soon as it is known.
                   Used by run-attacks.sh to point a watcher at a sender it just started.
  check <workdir>  read every scenario's sender and watcher files and print a verdict per scenario,
                   exiting non-zero if any failed.

The expectations are written here rather than in the shell, so what each scenario proves is stated in
one place and in words."""

import json
import pathlib
import sys


def events(path):
    out = []
    try:
        for line in pathlib.Path(path).read_text().splitlines():
            line = line.strip()
            if line:
                out.append(json.loads(line))
    except FileNotFoundError:
        pass
    return out


def own_session(path):
    for e in events(path):
        if e.get("event") == "synced":
            return e.get("session", 0)
    return 0


def summary(path):
    for e in events(path):
        if e.get("event") == "summary":
            return e
    return {}


def watcher_units(workdir, label):
    return summary(f"{workdir}/{label}.watcher.jsonl").get("recv_units", 0)


def max_source_name_len(workdir, label):
    # The length the watcher saw on the relayed VideoState for the stream it was watching.
    longest = 0
    for e in events(f"{workdir}/{label}.watcher.jsonl"):
        if e.get("event") == "video_state":
            longest = max(longest, e.get("source_name_len", 0))
    return longest


def check(workdir):
    results = []

    def record(name, ok, detail):
        results.append((name, ok, detail))

    # 1. Baseline control: the ordinary share must reach the ordinary watcher.
    n = watcher_units(workdir, "baseline-audio")
    record("baseline-audio", n > 0,
           f"watcher received {n} units (must be > 0, or the rest of the suite is meaningless)")

    # 2. A self-muted sender's audio must not reach the watcher.
    n = watcher_units(workdir, "muted-audio")
    record("muted-audio", n == 0,
           f"watcher received {n} units from a self-muted audio sender (must be 0)")

    # 3. A self-deafened watcher must receive no share audio.
    n = watcher_units(workdir, "deaf-watcher")
    record("deaf-watcher", n == 0,
           f"self-deafened watcher received {n} audio units (must be 0)")

    # 4. The mute must not touch picture: a self-muted sender's video still gets through.
    n = watcher_units(workdir, "muted-picture")
    record("muted-picture", n > 0,
           f"watcher received {n} units of a self-muted sender's PICTURE (must be > 0: mute gates sound only)")

    # 5. source_name is truncated to <= 512 bytes by the server, from the 4000 the sender sent.
    seen = max_source_name_len(workdir, "longname")
    record("source-name-truncated", 0 < seen <= 512,
           f"watcher saw a source_name of {seen} bytes (sender sent 4000; server must cap at 512)")

    width = max(len(name) for name, _, _ in results)
    failed = 0
    for name, ok, detail in results:
        mark = "PASS" if ok else "FAIL"
        if not ok:
            failed += 1
        print(f"  [{mark}] {name.ljust(width)}  {detail}")

    print(f"\n{len(results) - failed}/{len(results)} scenarios passed")
    return 1 if failed else 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "session":
        print(own_session(argv[2]))
        return 0
    if len(argv) >= 3 and argv[1] == "check":
        return check(argv[2])
    print("usage: attack-report.py session <file> | check <workdir>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
