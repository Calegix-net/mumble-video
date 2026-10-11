#!/bin/bash
# Builds the Windows wizard installer from a bundle directory, using NSIS.
#
# Run inside the Windows build image (docker/Dockerfile.windows). This is the installer a person
# should download; the MSI beside it exists for deployment tooling that wants one.
#
# Usage: build-nsis.sh <bundle-dir> <output.exe> <version>
#
# <version> is what Settings > Apps shows, e.g. 1.9.0 or 1.9.0-dev+abc123456 (the second word
# scripts/installer-version.sh prints).

set -euo pipefail

BUNDLE_DIR="$(realpath "${1:?usage: build-nsis.sh <bundle-dir> <output.exe> <version>}")"
OUT_EXE="${2:?usage: build-nsis.sh <bundle-dir> <output.exe> <version>}"
VERSION="${3:?usage: build-nsis.sh <bundle-dir> <output.exe> <version>}"

REPO_DIR="$(realpath "$(dirname "$0")/..")"

# NSIS takes Windows-style separators in File /r paths even when cross-building.
makensis -V2 \
    -DOUT_FILE="$(realpath -m "${OUT_EXE}")" \
    -DBUNDLE_DIR="${BUNDLE_DIR}" \
    -DLICENSE_FILE="${REPO_DIR}/LICENSE" \
    -DVERSION="${VERSION}" \
    "${REPO_DIR}/installer/mumble-video.nsi"

echo "built $(du -h "${OUT_EXE}" | cut -f1) installer: ${OUT_EXE} (version ${VERSION})"
