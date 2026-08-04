#!/bin/bash
# Configures and builds the Windows client inside the MinGW cross-build image.
#
# Run this from inside the container (see docker/Dockerfile.windows), with the source tree at /src and a
# writable build directory at /build.
#
# The compiler paths are given in full rather than left to the mingw64-cmake wrapper. CMake re-runs
# itself from inside make whenever a CMakeLists.txt changes, and that re-run does not inherit the
# wrapper's environment - with a bare compiler name in the cache it fails to find the toolchain and the
# build stops on a configure error that has nothing to do with what was edited.

set -euo pipefail

SRC_DIR="${1:-/src}"
BUILD_DIR="${2:-/build}"

mingw64-cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/bin/x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=/usr/bin/x86_64-w64-mingw32-g++ \
    -DCMAKE_RC_COMPILER=/usr/bin/x86_64-w64-mingw32-windres \
    -Dserver=OFF -Dtests=OFF -Dbenchmarks=OFF -Dice=OFF \
    -Doverlay=OFF -Doverlay-xcompile=OFF \
    -Dg15=OFF -Dasio=OFF -Dupdate=OFF -Dzeroconf=OFF -Djackaudio=OFF -Dspeechd=OFF \
    -Dtranslations=OFF

cmake --build "${BUILD_DIR}" -j"$(nproc)"
