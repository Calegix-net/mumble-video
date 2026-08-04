# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set(MUMBLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")

find_pkg(Qt6 COMPONENTS Gui REQUIRED)

set(TESTVIDEOPIPELINE_SOURCES
	TestVideoPipeline.cpp

	"${MUMBLE_SOURCE_DIR}/VideoEncoder.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoEncoder.h"
	"${MUMBLE_SOURCE_DIR}/VideoSource.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoSource.h"
)

add_executable(TestVideoPipeline ${TESTVIDEOPIPELINE_SOURCES})

set_target_properties(TestVideoPipeline PROPERTIES AUTOMOC ON)

target_include_directories(TestVideoPipeline PRIVATE ${MUMBLE_SOURCE_DIR})

target_link_libraries(TestVideoPipeline PRIVATE shared Qt6::Gui Qt6::Test)

add_test(NAME TestVideoPipeline COMMAND $<TARGET_FILE:TestVideoPipeline>)
