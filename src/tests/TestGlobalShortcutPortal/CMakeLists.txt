# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set(MUMBLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")

find_pkg(Qt6 COMPONENTS DBus REQUIRED)

add_executable(TestGlobalShortcutPortal
	TestGlobalShortcutPortal.cpp

	"${MUMBLE_SOURCE_DIR}/GlobalShortcutPortal.cpp"
	"${MUMBLE_SOURCE_DIR}/GlobalShortcutPortal.h"
)

set_target_properties(TestGlobalShortcutPortal PROPERTIES AUTOMOC ON)

target_include_directories(TestGlobalShortcutPortal PRIVATE ${MUMBLE_SOURCE_DIR})

target_link_libraries(TestGlobalShortcutPortal PRIVATE shared Qt6::DBus Qt6::Test)

# Starts a private dbus-daemon of its own, so it needs no session bus and never touches the real portal.
add_test(NAME TestGlobalShortcutPortal COMMAND $<TARGET_FILE:TestGlobalShortcutPortal>)
