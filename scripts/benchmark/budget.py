#!/usr/bin/env python3
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

"""Checks a benchmark run against the budgets in budgets.json, and fails the build if it is over.

    budget.py <run-label> [budgets.json]

The point of this existing at all: a pipeline has no compiler to tell it that latency went up. Every
regression in this code so far has been found by somebody deciding to measure, which means the ones
nobody measured are still here. A budget is the cheap half of that - it will not find a subtle
regression, but it will not let a gross one through, and it costs a minute.

What it deliberately does not do is compare against a previous run. A CI runner is shared, and a
comparison between two different runners is a comparison of two runners. Absolute limits with real
headroom say something true on any machine; a two per cent difference does not.

Prints every budget with its measured value, so a failure names the number instead of only the rule.
"""

import json
import pathlib
import sys

# Its own directory, explicitly: python -I and python -P both omit it, and a benchmark script that
# fails to import its neighbour depending on how it was invoked is a bad surprise in a CI log.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import benchpaths  # noqa: E402  (after the path insert, necessarily)

HERE = pathlib.Path(__file__).resolve().parent


def dig(row, path):
    """Follows a dotted metric path into a results row, or None if any step is missing."""
    node = row

    for key in path.split("."):
        if not isinstance(node, dict) or key not in node:
            return None

        node = node[key]

    return node if isinstance(node, (int, float)) else None


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().split("\n")[2].strip())
        return 2

    label = sys.argv[1]
    budgets_path = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "budgets.json"
    out = benchpaths.output_directory()
    results_path = out / label / "results.json"

    if not results_path.exists():
        print("no results at %s" % results_path)
        return 2

    rows = {row["scenario"]: row for row in json.loads(results_path.read_text())}
    budgets = json.loads(budgets_path.read_text())["budgets"]

    print("%-38s %-9s %10s %10s  %s" % ("metric", "scenario", "measured", "budget", "verdict"))
    failures = []
    missing = []

    for budget in budgets:
        metric, scenario = budget["metric"], budget["scenario"]
        row = rows.get(scenario)
        value = dig(row, metric) if row else None

        if value is None:
            # Not a pass. A metric that stopped being reported is a budget that silently stopped
            # being checked, which is worse than one that fails.
            missing.append((metric, scenario))
            print("%-38s %-9s %10s %10s  MISSING" % (metric, scenario, "-", "-"))
            continue

        if "at_least" in budget:
            limit, over = budget["at_least"], value < budget["at_least"]
            shown = ">= %.4g" % limit
        else:
            limit, over = budget["at_most"], value > budget["at_most"]
            shown = "<= %.4g" % limit

        print("%-38s %-9s %10.4g %10s  %s" % (metric, scenario, value, shown, "OVER" if over else "ok"))

        if over:
            failures.append((metric, scenario, value, shown, budget["why"]))

    print()

    if missing:
        print("%d budget(s) could not be checked because the metric was not reported:" % len(missing))

        for metric, scenario in missing:
            print("  %s (%s)" % (metric, scenario))

        print("Either the run did not get that far, or the summariser stopped emitting it.")

    for metric, scenario, value, shown, why in failures:
        print()
        print("OVER BUDGET: %s on %s is %.4g, budget %s" % (metric, scenario, value, shown))
        print("  %s" % why)

    if failures or missing:
        return 1

    print("all %d budgets met" % len(budgets))

    return 0


if __name__ == "__main__":
    sys.exit(main())
