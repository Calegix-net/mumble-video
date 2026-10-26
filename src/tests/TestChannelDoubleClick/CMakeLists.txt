# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

find_pkg(Qt6 COMPONENTS Widgets REQUIRED)

add_executable(TestChannelDoubleClick TestChannelDoubleClick.cpp)
target_link_libraries(TestChannelDoubleClick PRIVATE mumble_client_object_lib Qt6::Test)
set_target_properties(TestChannelDoubleClick PROPERTIES AUTOMOC ON)

add_test(NAME TestChannelDoubleClick COMMAND $<TARGET_FILE:TestChannelDoubleClick> WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
# A real tree view, on a platform that needs no display.
set_tests_properties(TestChannelDoubleClick PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
