#!/bin/bash
# Stages the built server binary into a directory that docker/Dockerfile.server can be built against.
#
# Run from inside the Linux build container (docker/Dockerfile), with the project already built at
# BUILD_DIR. Produces OUT_DIR/bin/mumble-server and OUT_DIR/lib. The runtime image resolves most shared
# libraries the binary needs from its own dnf packages; ZeroC Ice is not packaged by Fedora, so when the
# server was built with Ice the libraries it links (from build-ice.sh) are staged into OUT_DIR/lib.
set -euo pipefail

BUILD_DIR="${1:-/build}"
OUT_DIR="${2:-/out/server}"

if [ ! -x "${BUILD_DIR}/mumble-server" ]; then
    echo "bundle-server: no mumble-server binary at ${BUILD_DIR}/mumble-server - was the project built with -Dserver=ON?" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}/bin" "${OUT_DIR}/lib"
cp "${BUILD_DIR}/mumble-server" "${OUT_DIR}/bin/mumble-server"

# Each libIce*.so.37 the binary resolves, copied under its soname. -L follows the soname symlink.
ldd "${BUILD_DIR}/mumble-server" | awk '$1 ~ /^libIce/ && $3 ~ /^\// { print $3 }' | while read -r lib; do
    cp -L "${lib}" "${OUT_DIR}/lib/$(basename "${lib}")"
done
echo "bundle-server: staged $(du -h "${OUT_DIR}/bin/mumble-server" | cut -f1) server binary at ${OUT_DIR}/bin/mumble-server"
echo "bundle-server: staged $(find "${OUT_DIR}/lib" -type f | wc -l) Ice libraries at ${OUT_DIR}/lib"
