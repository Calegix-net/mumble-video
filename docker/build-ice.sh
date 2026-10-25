#!/bin/bash
# Builds the ZeroC Ice the server's Ice RPC interface links (src/murmur/MumbleServerIce.cpp), which Fedora
# does not package. Only the C++98-mapping shared libraries the server finds (Ice, IceSSL, IceDiscovery,
# IceLocatorDiscovery) and the slice2cpp compiler are built - no IceGrid, IceStorm, Glacier2 or tests.
# slice2cpp needs ZeroC's fork of mcpp, which Fedora does not package either, so it is built first.
#
# Usage: build-ice.sh <prefix>
#   Installs bin/slice2cpp, include/, lib64/ and share/ice/slice under <prefix>.
set -euo pipefail

PREFIX="${1:?usage: build-ice.sh <prefix>}"

ICE_VERSION=v3.7.11
MCPP_COMMIT=6a6551b971f781353b20006302de538609491a76

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

git -c advice.detachedHead=false clone -q --depth 1 --branch "${ICE_VERSION}" https://github.com/zeroc-ice/ice.git "${WORK}/ice"
git init -q "${WORK}/mcpp"
git -C "${WORK}/mcpp" fetch -q --depth 1 https://github.com/zeroc-ice/mcpp.git "${MCPP_COMMIT}"
git -C "${WORK}/mcpp" -c advice.detachedHead=false checkout -q FETCH_HEAD

# mcpp is linked statically into slice2cpp, so nothing of it is installed.
make -C "${WORK}/mcpp" -j"$(nproc)" >/dev/null

ice_make=(
    -C "${WORK}/ice/cpp"
    CONFIGS=shared OPTIMIZE=yes PLATFORMS=x64
    MCPP_HOME="${WORK}/mcpp" MCPP_LIBDIR="${WORK}/mcpp/lib64"
    prefix="${PREFIX}"
    # Ice 3.7 predates GCC 13+'s new warnings and builds with -Werror.
    CXXFLAGS="-Wno-error -Wno-deprecated-declarations"
)

make "${ice_make[@]}" -j"$(nproc)" slice2cpp Ice IceSSL IceDiscovery IceLocatorDiscovery >/dev/null
make "${ice_make[@]}" slice2cpp_install IceUtil_install Ice_install IceSSL_install IceDiscovery_install \
    IceLocatorDiscovery_install >/dev/null
# The .ice files slice2cpp includes while compiling MumbleServer.ice.
make -C "${WORK}/ice" prefix="${PREFIX}" install-slice >/dev/null

echo "build-ice: installed Ice ${ICE_VERSION} under ${PREFIX}"
