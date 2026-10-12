#!/bin/bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.
#
# Builds what mumble-core links against on Android, for one ABI, as static libraries under PREFIX:
# libcrypto from OpenSSL, and the Protobuf lite runtime with the Abseil it needs.
#
#   build-deps.sh ABI PREFIX        ABI: arm64-v8a, x86_64, armeabi-v7a or x86
#
# Environment:
#   ANDROID_NDK_ROOT   the NDK (ANDROID_NDK_HOME works too)
#   ANDROID_API        minimum API level, default 29
#   OPENSSL_VERSION    default 3.5.9
#   PROTOBUF_VERSION   default: the version of the protoc on PATH. The generated code is only valid
#                      against the runtime of the same version, so the two must match exactly.
#   DEPS_SRC           where sources are downloaded, default PREFIX/../src
set -euo pipefail

ABI="${1:?usage: $0 ABI PREFIX}"
PREFIX="$(mkdir -p "${2:?usage: $0 ABI PREFIX}" && cd "$2" && pwd)"
NDK="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:?set ANDROID_NDK_ROOT}}"
API="${ANDROID_API:-29}"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.9}"
PROTOBUF_VERSION="${PROTOBUF_VERSION:-$(protoc --version | awk '{print $2}')}"
DEPS_SRC="${DEPS_SRC:-$(dirname "$PREFIX")/src}"
JOBS="$(nproc)"

case "$ABI" in
	arm64-v8a) OPENSSL_TARGET=android-arm64 ;;
	x86_64) OPENSSL_TARGET=android-x86_64 ;;
	armeabi-v7a) OPENSSL_TARGET=android-arm ;;
	x86) OPENSSL_TARGET=android-x86 ;;
	*) echo "unknown ABI $ABI" >&2; exit 2 ;;
esac

mkdir -p "$DEPS_SRC"

# OpenSSL: libcrypto only. The core uses AES-128-ECB, SHA-1/256 and RAND_bytes, nothing of TLS (the
# app's own TLS stack carries the control channel).
openssl_src="$DEPS_SRC/openssl-$OPENSSL_VERSION"
if [ ! -d "$openssl_src" ]; then
	curl -fsSL "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz" \
		| tar -xz -C "$DEPS_SRC"
fi
openssl_build="$DEPS_SRC/build-openssl-$ABI"
rm -rf "$openssl_build"
cp -a "$openssl_src" "$openssl_build"
(
	cd "$openssl_build"
	export ANDROID_NDK_ROOT="$NDK"
	export PATH="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
	./Configure "$OPENSSL_TARGET" -D__ANDROID_API__="$API" --prefix="$PREFIX" --libdir=lib \
		no-shared no-module no-tests no-apps no-docs no-ssl no-engine no-dso
	make -j"$JOBS" build_libs >/dev/null
	make install_dev >/dev/null
)
rm -rf "$openssl_build"

# Protobuf, lite runtime only, with Abseil built from its bundled copy.
protobuf_src="$DEPS_SRC/protobuf-$PROTOBUF_VERSION"
if [ ! -d "$protobuf_src" ]; then
	git clone -q --depth 1 --branch "v$PROTOBUF_VERSION" --recurse-submodules --shallow-submodules \
		https://github.com/protocolbuffers/protobuf.git "$protobuf_src"
fi
protobuf_build="$DEPS_SRC/build-protobuf-$ABI"
cmake -S "$protobuf_src" -B "$protobuf_build" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DCMAKE_CXX_STANDARD=20 \
	-Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_CONFORMANCE=OFF -Dprotobuf_BUILD_EXAMPLES=OFF \
	-Dprotobuf_BUILD_PROTOC_BINARIES=OFF -Dprotobuf_BUILD_LIBUPB=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF \
	-Dprotobuf_INSTALL=ON -DABSL_PROPAGATE_CXX_STD=ON -DABSL_ENABLE_INSTALL=ON \
	>/dev/null
cmake --build "$protobuf_build" -j"$JOBS" >/dev/null
cmake --install "$protobuf_build" >/dev/null
rm -rf "$protobuf_build"

echo "OpenSSL $OPENSSL_VERSION and Protobuf $PROTOBUF_VERSION for $ABI are in $PREFIX"
