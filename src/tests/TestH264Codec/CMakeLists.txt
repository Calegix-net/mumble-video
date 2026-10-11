# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set(MUMBLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")

find_pkg(Qt6 COMPONENTS Gui REQUIRED)

# Imported targets from pkg_check_modules are scoped to the directory that created them, so this has to
# be looked up here as well as in the client build.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)

set(TESTH264CODEC_SOURCES
	TestH264Codec.cpp

	"${MUMBLE_SOURCE_DIR}/H264Codec.cpp"
	"${MUMBLE_SOURCE_DIR}/H264Codec.h"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.h"
)

add_executable(TestH264Codec ${TESTH264CODEC_SOURCES})

set_target_properties(TestH264Codec PROPERTIES AUTOMOC ON)

target_include_directories(TestH264Codec PRIVATE ${MUMBLE_SOURCE_DIR})

target_link_libraries(TestH264Codec PRIVATE shared Qt6::Gui Qt6::Test PkgConfig::LIBAV)

add_test(NAME TestH264Codec COMMAND $<TARGET_FILE:TestH264Codec>)
