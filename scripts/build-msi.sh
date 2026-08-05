#!/bin/bash
# Builds the Windows MSI from a bundle directory, using wixl from msitools.
#
# Run inside the Windows build image (docker/Dockerfile.windows). The bundle is what
# scripts/bundle-windows.sh produced: mumble.exe with every DLL and plugin beside it. The file list is
# harvested from the directory rather than maintained by hand, so the MSI always contains exactly what
# the zip contains.
#
# Usage: build-msi.sh <bundle-dir> <output.msi> [version]

set -euo pipefail

BUNDLE_DIR="${1:?usage: build-msi.sh <bundle-dir> <output.msi> [version]}"
OUT_MSI="${2:?usage: build-msi.sh <bundle-dir> <output.msi> [version]}"
VERSION="${3:-1.7.0}"

WXS_DIR="$(dirname "$0")/../installer"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# wixl-heat writes one component per file into a fragment rooted at INSTALLDIR. The prefix strips the
# bundle path so files install to their bundle-relative locations.
(cd "${BUNDLE_DIR}" && find . -type f) \
    | wixl-heat --prefix "./" \
                --component-group CG.Bundle \
                --var var.SourceDir \
                --directory-ref INSTALLDIR \
                --win64 \
    > "${WORK}/bundle-files.wxs"

wixl --arch x64 \
     -D Win64=yes \
     -D SourceDir="${BUNDLE_DIR}" \
     -D Version="${VERSION}" \
     -o "${OUT_MSI}" \
     "${WXS_DIR}/mumble-video.wxs" "${WORK}/bundle-files.wxs"

echo "built $(du -h "${OUT_MSI}" | cut -f1) MSI: ${OUT_MSI} (version ${VERSION})"
