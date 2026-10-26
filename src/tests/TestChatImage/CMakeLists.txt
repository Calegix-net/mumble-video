# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set(MUMBLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")

find_pkg(Qt6 COMPONENTS Gui REQUIRED)

set(TESTCHATIMAGE_SOURCES
	TestChatImage.cpp

	"${MUMBLE_SOURCE_DIR}/ChatImage.cpp"
	"${MUMBLE_SOURCE_DIR}/ChatImage.h"
	"${MUMBLE_SOURCE_DIR}/LogDocument.cpp"
	"${MUMBLE_SOURCE_DIR}/LogDocument.h"
)

add_executable(TestChatImage ${TESTCHATIMAGE_SOURCES})

set_target_properties(TestChatImage PROPERTIES AUTOMOC ON)

target_include_directories(TestChatImage PRIVATE ${MUMBLE_SOURCE_DIR})

target_link_libraries(TestChatImage PRIVATE Qt6::Gui Qt6::Test)

add_test(NAME TestChatImage COMMAND $<TARGET_FILE:TestChatImage> -platform offscreen)
set_tests_properties(TestChatImage PROPERTIES TIMEOUT 120)
