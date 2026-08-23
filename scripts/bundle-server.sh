#!/bin/bash
# Stages the built server binary into a directory that docker/Dockerfile.server can be built against.
#
# Run from inside the Linux build container (docker/Dockerfile), with the project already built at
# BUILD_DIR. Produces OUT_DIR/bin/mumble-server. The runtime image resolves the shared libraries the
# binary needs from its own dnf packages, so only the binary itself is staged here.
set -euo pipefail

BUILD_DIR="${1:-/build}"
OUT_DIR="${2:-/out/server}"

if [ ! -x "${BUILD_DIR}/mumble-server" ]; then
    echo "bundle-server: no mumble-server binary at ${BUILD_DIR}/mumble-server - was the project built with -Dserver=ON?" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}/bin"
cp "${BUILD_DIR}/mumble-server" "${OUT_DIR}/bin/mumble-server"
echo "bundle-server: staged $(du -h "${OUT_DIR}/bin/mumble-server" | cut -f1) server binary at ${OUT_DIR}/bin/mumble-server"
