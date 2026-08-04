# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set(MURMUR_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/murmur")

set(TESTVIDEOROUTER_SOURCES
	TestVideoRouter.cpp

	"${MURMUR_SOURCE_DIR}/VideoRouter.cpp"
	"${MURMUR_SOURCE_DIR}/VideoRouter.h"
)

add_executable(TestVideoRouter ${TESTVIDEOROUTER_SOURCES})

set_target_properties(TestVideoRouter PROPERTIES AUTOMOC ON)

target_include_directories(TestVideoRouter PRIVATE ${MURMUR_SOURCE_DIR})

target_link_libraries(TestVideoRouter PRIVATE shared Qt6::Test)

add_test(NAME TestVideoRouter COMMAND $<TARGET_FILE:TestVideoRouter>)
