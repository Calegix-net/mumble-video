# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set(MUMBLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")

find_pkg(Qt6 COMPONENTS Gui REQUIRED)

# Imported targets from pkg_check_modules are scoped to the directory that created them, so this has to
# be looked up here as well as in the client build.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VPX REQUIRED IMPORTED_TARGET vpx)

set(TESTVP8CODEC_SOURCES
	TestVP8Codec.cpp

	"${MUMBLE_SOURCE_DIR}/VP8Codec.cpp"
	"${MUMBLE_SOURCE_DIR}/VP8Codec.h"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.h"
)

add_executable(TestVP8Codec ${TESTVP8CODEC_SOURCES})

set_target_properties(TestVP8Codec PROPERTIES AUTOMOC ON)

target_include_directories(TestVP8Codec PRIVATE ${MUMBLE_SOURCE_DIR})

target_link_libraries(TestVP8Codec PRIVATE shared Qt6::Gui Qt6::Test PkgConfig::VPX)

add_test(NAME TestVP8Codec COMMAND $<TARGET_FILE:TestVP8Codec>)
