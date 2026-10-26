# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set(MUMBLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")

add_executable(TestScreenAwake
	TestScreenAwake.cpp
	"${MUMBLE_SOURCE_DIR}/ScreenAwake.cpp"
	"${MUMBLE_SOURCE_DIR}/ScreenAwake.h"
)

set_target_properties(TestScreenAwake PROPERTIES AUTOMOC ON)

target_include_directories(TestScreenAwake PRIVATE ${MUMBLE_SOURCE_DIR})

target_link_libraries(TestScreenAwake PRIVATE shared Qt6::Test)

if(dbus AND (NOT WIN32 AND NOT APPLE))
	find_pkg(Qt6 COMPONENTS DBus REQUIRED)
	target_compile_definitions(TestScreenAwake PRIVATE "USE_DBUS")
	target_link_libraries(TestScreenAwake PRIVATE Qt6::DBus)
endif()

add_test(NAME TestScreenAwake COMMAND $<TARGET_FILE:TestScreenAwake>)
