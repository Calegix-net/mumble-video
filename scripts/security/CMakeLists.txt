# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# The security test client, mv-attack. Built on its own, not as part of the main tree: it is a
# verification tool for a running server, in the same spirit as scripts/benchmark, and it has no place
# in a client or server binary.
#
# It builds mumble-core as a subdirectory, which is the supported standalone configuration, so it links
# the real control framer, UDP codecs and OCB2 video crypt rather than a reimplementation. That also
# gives it the generated Mumble.pb.h / MumbleUDP.pb.h and their include directory for free.
#
# Configure against the repo's mumble-core:
#   cmake -S scripts/security -B <build> -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build <build>

cmake_minimum_required(VERSION 3.16)
project(mv-attack CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# mumble-core lives two directories up, under src/.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/mumble-core" "${CMAKE_BINARY_DIR}/mumble-core")

find_package(OpenSSL REQUIRED)

add_executable(mv-attack "mv-attack.cpp")
target_link_libraries(mv-attack PRIVATE mumble-core OpenSSL::SSL OpenSSL::Crypto)

# mumble-core is Qt-free and so is this, which is the point: the tool builds and runs anywhere the
# protocol core does, including a CI runner with no display and no Qt.
