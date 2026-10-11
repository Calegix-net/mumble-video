#!/bin/bash
# Prints the version the Windows installers are stamped with, as two words: the MSI ProductVersion
# (three numbers, which Windows Installer compares to decide upgrades) and the version shown to people
# in Settings > Apps.
#
# A v* tag build is that release. Anything else - main, a pull request - is a development build after
# the newest release, so it carries that release's number plus "-dev" and the commit. Its MSI version
# equals the release's, and the MSI treats an equal version as an upgrade, so a development build and
# its release replace each other in either order while the next release still replaces both.
#
# Runs on the CI runner, not in a build container: it asks the remote for tags, which a shallow
# checkout does not have.
#
# Usage: installer-version.sh

set -euo pipefail

if [[ "${GITHUB_REF:-}" == refs/tags/v* ]]; then
    tag="${GITHUB_REF#refs/tags/}"
    suffix=""
else
    tag="$(git ls-remote --tags --refs origin \
        | awk '{ sub("refs/tags/", "", $2); print $2 }' \
        | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' \
        | sort -V | tail -n 1)"
    suffix="-dev+$(git rev-parse --short=9 HEAD)"
fi

if [[ ! "${tag}" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    echo "installer-version: no usable vX.Y.Z tag (got '${tag}')" >&2
    exit 1
fi

major="${BASH_REMATCH[1]}"
minor="${BASH_REMATCH[2]}"
patch="${BASH_REMATCH[3]}"

# Windows Installer's limits for ProductVersion fields.
if (( major > 255 || minor > 255 || patch > 65535 )); then
    echo "installer-version: ${tag} does not fit an MSI ProductVersion" >&2
    exit 1
fi

echo "${major}.${minor}.${patch} ${major}.${minor}.${patch}${suffix}"
