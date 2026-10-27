# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

"""Where benchmark runs live, worked out the same way env.sh works it out.

This exists because the Python here is not always a child of a shell that sourced env.sh. `bench.sh`
sources it and exports, so a script it calls inherits `MV_OUT`; a CI step that runs `budget.py`
directly is its own process and inherits nothing. Having the rule in one place, agreeing with env.sh,
is the difference between that working and `budget.py` reporting "no results" for a run sitting in
front of it - which is exactly how this was found.

The order matches env.sh: the explicit variable wins, then the user-facing work directory, then the
cache directory, then its default.
"""

import os
import pathlib


def output_directory():
    """The directory holding one subdirectory per run."""
    explicit = os.environ.get("MV_OUT")

    if explicit:
        return pathlib.Path(explicit)

    work = os.environ.get("MUMBLE_BENCH_WORK")

    if work:
        return pathlib.Path(work) / "out"

    cache = os.environ.get("XDG_CACHE_HOME")
    base = pathlib.Path(cache) if cache else pathlib.Path.home() / ".cache"

    return base / "mumble-video-bench" / "out"
