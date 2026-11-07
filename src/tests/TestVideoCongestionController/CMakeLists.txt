# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# VideoCongestionController is a self-contained, Qt-free control law, so the test compiles it directly
# rather than linking the whole client.
set(MUMBLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")

add_executable(TestVideoCongestionController
	TestVideoCongestionController.cpp
	"${MUMBLE_SOURCE_DIR}/VideoCongestionController.cpp"
	"${MUMBLE_SOURCE_DIR}/VideoCongestionController.h"
)

set_target_properties(TestVideoCongestionController PROPERTIES AUTOMOC ON)

target_include_directories(TestVideoCongestionController PRIVATE "${MUMBLE_SOURCE_DIR}")

target_link_libraries(TestVideoCongestionController PRIVATE Qt6::Test)

add_test(NAME TestVideoCongestionController COMMAND $<TARGET_FILE:TestVideoCongestionController>)
