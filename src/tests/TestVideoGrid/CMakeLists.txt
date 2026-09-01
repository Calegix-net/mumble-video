# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set(MUMBLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")

find_pkg(Qt6 COMPONENTS Concurrent Gui Widgets REQUIRED)

# The grid decodes VP8 itself now, so it needs the codec and libvpx. Imported targets from
# pkg_check_modules are scoped to the directory that creates them, hence the lookup here.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VPX REQUIRED IMPORTED_TARGET vpx)

set(TESTVIDEOGRID_SOURCES
	TestVideoGrid.cpp

	"${MUMBLE_SOURCE_DIR}/VP8Codec.cpp"
	"${MUMBLE_SOURCE_DIR}/VP8Codec.h"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.h"
	"${MUMBLE_SOURCE_DIR}/VideoGrid.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoGrid.h"
	"${MUMBLE_SOURCE_DIR}/VideoSource.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoSource.h"
)

add_executable(TestVideoGrid ${TESTVIDEOGRID_SOURCES})

set_target_properties(TestVideoGrid PROPERTIES AUTOMOC ON)

target_include_directories(TestVideoGrid PRIVATE ${MUMBLE_SOURCE_DIR})

target_link_libraries(TestVideoGrid PRIVATE shared Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::Test PkgConfig::VPX)

add_test(NAME TestVideoGrid COMMAND $<TARGET_FILE:TestVideoGrid>)
