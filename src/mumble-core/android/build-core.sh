#!/bin/bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.
#
# Builds mumble-core (and TestMumbleCore) for one Android ABI against dependencies from build-deps.sh.
#
#   build-core.sh ABI PREFIX BUILD_DIR
#
# The same settings are what an app's own CMake build (Gradle externalNativeBuild) needs to pass when it
# adds src/mumble-core with add_subdirectory(). See ../README.md.
set -euo pipefail

ABI="${1:?usage: $0 ABI PREFIX BUILD_DIR}"
PREFIX="$(cd "${2:?usage: $0 ABI PREFIX BUILD_DIR}" && pwd)"
BUILD="${3:?usage: $0 ABI PREFIX BUILD_DIR}"
NDK="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:?set ANDROID_NDK_ROOT}}"
API="${ANDROID_API:-29}"
CORE="$(cd "$(dirname "$0")/.." && pwd)"

cmake -S "$CORE" -B "$BUILD" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_FIND_ROOT_PATH="$PREFIX" -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=ON \
	-DMUMBLE_CORE_PROTOBUF_LITE=ON -DMUMBLE_CORE_LEGACY_UDP=OFF \
	-DCMAKE_CXX_FLAGS="-Wall -Wextra -Wsign-conversion -Werror"
cmake --build "$BUILD" -j"$(nproc)"
