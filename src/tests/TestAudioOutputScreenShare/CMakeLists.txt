# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(TestAudioOutputScreenShare TestAudioOutputScreenShare.cpp)

set_target_properties(TestAudioOutputScreenShare PROPERTIES AUTOMOC ON)

target_link_libraries(TestAudioOutputScreenShare PRIVATE mumble_client_object_lib Qt6::Test)

add_test(
	NAME TestAudioOutputScreenShare
	COMMAND $<TARGET_FILE:TestAudioOutputScreenShare>
	# Specifying the working directory is necessary, to make sure the dependent DLLs are found (on Windows)
	WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
