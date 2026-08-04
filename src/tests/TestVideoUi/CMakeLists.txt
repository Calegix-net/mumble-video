# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set(MUMBLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")

find_pkg(Qt6 COMPONENTS Gui REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(VPX REQUIRED IMPORTED_TARGET vpx)

set(TESTVIDEOUI_SOURCES
	TestVideoUi.cpp

	"${MUMBLE_SOURCE_DIR}/VP8Codec.cpp"
	"${MUMBLE_SOURCE_DIR}/VP8Codec.h"
	"${MUMBLE_SOURCE_DIR}/VideoBandwidthProbe.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoBandwidthProbe.h"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.h"
	"${MUMBLE_SOURCE_DIR}/VideoSource.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoSource.h"
)

add_executable(TestVideoUi ${TESTVIDEOUI_SOURCES})

set_target_properties(TestVideoUi PROPERTIES AUTOMOC ON)

target_include_directories(TestVideoUi PRIVATE ${MUMBLE_SOURCE_DIR})

target_link_libraries(TestVideoUi PRIVATE shared Qt6::Gui Qt6::Test PkgConfig::VPX)

# The sweep encodes for a couple of seconds per candidate.
add_test(NAME TestVideoUi COMMAND $<TARGET_FILE:TestVideoUi>)
set_tests_properties(TestVideoUi PROPERTIES TIMEOUT 120)
