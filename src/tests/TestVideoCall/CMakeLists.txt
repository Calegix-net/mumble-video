# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestVideoCall TestVideoCall.cpp)

set_target_properties(TestVideoCall PROPERTIES AUTOMOC ON)

target_link_libraries(TestVideoCall PRIVATE shared Qt6::Network Qt6::Test)

# The test starts the real server, so it needs to know where it ended up and must not run before it is
# built. If the server was not built at all the test skips rather than failing.
target_compile_definitions(TestVideoCall PRIVATE "MUMBLE_SERVER_BINARY=\"$<TARGET_FILE:mumble-server>\"")
add_dependencies(TestVideoCall mumble-server)

add_test(NAME TestVideoCall COMMAND $<TARGET_FILE:TestVideoCall>)

# It brings a process up and talks over real sockets, so it is slower than the rest of the suite.
set_tests_properties(TestVideoCall PROPERTIES TIMEOUT 120)
