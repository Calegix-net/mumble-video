#!/usr/bin/env python3
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

"""Puts function names on the frames of a perf profile that land in our own binary.

Why this exists at all
----------------------

A perf built without libelf cannot read a symbol table - any symbol table. Every frame in every
library comes back as ``[unknown]``, including libc's, and ``perf report`` prints raw hexadecimal
without a word of explanation. The kernel's perf on WSL2 is such a build, and a profile taken there
looks like a profile of a stripped binary even when the binary has twenty thousand symbols and full
debug information.

What perf still gives us, per frame, is the runtime address and the path of the file it was mapped
from, and it records an MMAP2 event with that file's load address and file offset. That is everything
symbolisation needs::

    ELF address = runtime address - (mapping start - mapping file offset)

and ``addr2line`` does the rest. So this script does the part perf declined to do, for the one image
it is worth doing for: ours. Nothing else in either profile has a debug package installed, and
``perf report --sort dso`` already answers the question for those - "a third of the sender is in
libswscale" needs no symbol name to act on.

It is also honest about what a frame-pointer profile of an optimised build can tell you: an inlined
function has no frame of its own, so ``addr2line`` is asked for the whole inline chain and the
innermost name is the one reported, with the chain shown when it is more than one deep.

    resolve.py <perf.data> <binary> [--limit N]
"""

import argparse
import collections
import os
import re
import subprocess
import sys

# "[0x5b2cc3e9b000(0x4cd000) @ 0xa3000 <buildid>]: r-xp /path/to/file"
MMAP = re.compile(r"\[0x([0-9a-f]+)\((?:0x[0-9a-f]+)\) @ 0x([0-9a-f]+)[^]]*\]: r-xp (\S+)")
# A callchain line: "\t    7663aff3d342 [unknown] (/usr/lib/.../libswscale.so.8.3.100)"
FRAME = re.compile(r"^\s+([0-9a-f]+)\s+.*\((\S+)\)\s*$")


def perf_script(data, extra=()):
    command = ["perf", "script", "-i", data] + list(extra)
    result = subprocess.run(command, capture_output=True, text=True)

    if result.returncode != 0:
        sys.exit("perf script failed: %s" % result.stderr.strip().splitlines()[-1:])

    return result.stdout


def load_base(data, binary):
    """The value to subtract from a runtime address in `binary` to get an ELF address."""
    real = os.path.realpath(binary)

    for line in perf_script(data, ["--show-mmap-events"]).splitlines():
        found = MMAP.search(line)

        if found and os.path.realpath(found.group(3)) == real:
            start, offset = int(found.group(1), 16), int(found.group(2), 16)
            return start - offset

    return None


def tally(data, binary, base):
    """Self and inclusive sample counts per ELF address in `binary`.

    Self is the leaf frame of a sample; inclusive is every distinct address on the stack, counted
    once per sample so that a recursive call does not count twice.
    """
    name = os.path.basename(binary)
    self_counts = collections.Counter()
    inclusive = collections.Counter()
    stack = []

    def flush():
        if not stack:
            return

        if stack[0] is not None:
            self_counts[stack[0]] += 1

        for address in {a for a in stack if a is not None}:
            inclusive[address] += 1

        del stack[:]

    for line in perf_script(data).splitlines():
        if not line.strip():
            flush()
            continue

        found = FRAME.match(line)

        if not found:
            # The sample's header line: comm, pid, timestamp, event.
            flush()
            continue

        mapped = found.group(2)
        ours = os.path.basename(mapped) == name or mapped.endswith("/" + name)
        stack.append(int(found.group(1), 16) - base if ours else None)

    flush()

    return self_counts, inclusive


def symbolise(binary, addresses):
    """One addr2line call for every address: it accepts a list and keeps the order."""
    if not addresses:
        return {}

    command = ["addr2line", "-f", "-C", "-i", "-e", binary] + ["0x%x" % a for a in addresses]
    result = subprocess.run(command, capture_output=True, text=True)
    lines = result.stdout.splitlines()

    # With -i, one address can produce several function/location pairs - the inline chain, outermost
    # last. addr2line gives no separator between addresses, so the only way to split the output is to
    # ask for one address at a time whenever any chain is longer than one. Two passes is still two
    # subprocesses rather than one per address.
    if len(lines) == 2 * len(addresses):
        pairs = [(lines[2 * i], lines[2 * i + 1]) for i in range(len(addresses))]
        return {a: [p] for a, p in zip(addresses, pairs)}

    out = {}

    for address in addresses:
        one = subprocess.run(
            ["addr2line", "-f", "-C", "-i", "-e", binary, "0x%x" % address],
            capture_output=True, text=True).stdout.splitlines()
        out[address] = [(one[i], one[i + 1]) for i in range(0, len(one) - 1, 2)]

    return out


def shorten(location):
    """Trim an absolute source path down to something that fits in a table."""
    location = location.split(" (discriminator")[0]

    for marker in ("/src/", "/plugins/", "/3rdparty/"):
        at = location.find(marker)

        if at != -1:
            return location[at + 1:]

    return location


def table(title, counts, total, resolved, limit):
    print(title)

    if not counts:
        print("  (no samples landed in this image)")
        print()
        return

    for address, count in counts.most_common(limit):
        chain = resolved.get(address) or [("??", "??")]
        function, location = chain[0]
        share = 100.0 * count / total if total else 0.0
        print("  %5.1f %%  %-44s %s" % (share, function[:44], shorten(location)))

        for function, location in chain[1:]:
            print("           %-44s %s" % ("  inlined into " + function[:30], shorten(location)))

    print()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("data", help="a perf.data file")
    parser.add_argument("binary", help="the binary to resolve addresses in")
    parser.add_argument("--limit", type=int, default=15, help="rows per table")
    arguments = parser.parse_args()

    base = load_base(arguments.data, arguments.binary)

    if base is None:
        sys.exit("%s was never mapped executable in %s; nothing to resolve"
                 % (arguments.binary, arguments.data))

    self_counts, inclusive = tally(arguments.data, arguments.binary, base)
    wanted = {a for a, _ in self_counts.most_common(arguments.limit)}
    wanted |= {a for a, _ in inclusive.most_common(arguments.limit)}
    resolved = symbolise(arguments.binary, sorted(wanted))

    # Every sample perf recorded, so that the percentages are shares of the whole profile rather than
    # of the part that happened to be ours - which would make a 5 % image look like 100 %.
    samples = sum(1 for line in perf_script(arguments.data).splitlines()
                  if line.strip() and not FRAME.match(line))

    print("load base 0x%x, %d samples in the profile, %d of them with %s as the leaf"
          % (base, samples, sum(self_counts.values()), os.path.basename(arguments.binary)))
    print()

    table("Self - the sample was taken in this function:", self_counts, samples, resolved,
          arguments.limit)
    table("Inclusive - this function was somewhere on the stack:", inclusive, samples, resolved,
          arguments.limit)

    return 0


if __name__ == "__main__":
    sys.exit(main())
