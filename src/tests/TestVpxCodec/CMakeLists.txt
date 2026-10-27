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

# The decoder converts I420 to RGB with libswscale when the client is built with H.264 support, and with
# a scalar loop otherwise. Both are correct and they must agree, so this suite builds the same way the
# client does - otherwise every colour test here would be exercising the fallback while users run the
# other one.
if(h264)
	pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
endif()

set(TESTVPXCODEC_SOURCES
	TestVpxCodec.cpp

	"${MUMBLE_SOURCE_DIR}/VpxCodec.cpp"
	"${MUMBLE_SOURCE_DIR}/VpxCodec.h"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.h"
)

add_executable(TestVpxCodec ${TESTVPXCODEC_SOURCES})

set_target_properties(TestVpxCodec PROPERTIES AUTOMOC ON)

target_include_directories(TestVpxCodec PRIVATE ${MUMBLE_SOURCE_DIR})

target_link_libraries(TestVpxCodec PRIVATE shared Qt6::Gui Qt6::Test PkgConfig::VPX)

if(h264)
	target_compile_definitions(TestVpxCodec PRIVATE "USE_H264")
	target_link_libraries(TestVpxCodec PRIVATE PkgConfig::LIBAV)
endif()

add_test(NAME TestVpxCodec COMMAND $<TARGET_FILE:TestVpxCodec>)
