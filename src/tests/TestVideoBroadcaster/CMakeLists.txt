# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set(MUMBLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")

find_pkg(Qt6 COMPONENTS Gui REQUIRED)

# VideoBroadcaster can encode with either codec, so the test needs VP8 too.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VPX REQUIRED IMPORTED_TARGET vpx)

set(TESTVIDEOBROADCASTER_SOURCES
	TestVideoBroadcaster.cpp

	"${MUMBLE_SOURCE_DIR}/VideoBroadcaster.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoBroadcaster.h"
	"${MUMBLE_SOURCE_DIR}/VP8Codec.cpp"
	"${MUMBLE_SOURCE_DIR}/VP8Codec.h"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.h"
	"${MUMBLE_SOURCE_DIR}/VideoSource.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoSource.h"
)

add_executable(TestVideoBroadcaster ${TESTVIDEOBROADCASTER_SOURCES})

set_target_properties(TestVideoBroadcaster PROPERTIES AUTOMOC ON)

target_include_directories(TestVideoBroadcaster PRIVATE ${MUMBLE_SOURCE_DIR})

target_link_libraries(TestVideoBroadcaster PRIVATE shared Qt6::Gui Qt6::Test PkgConfig::VPX)

add_test(NAME TestVideoBroadcaster COMMAND $<TARGET_FILE:TestVideoBroadcaster>)
