# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license.
find_package(PkgConfig REQUIRED)
pkg_check_modules(CAPTURE_PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
find_pkg(Qt6 COMPONENTS Gui DBus REQUIRED)
set(CAPTURE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")
add_executable(TestLinuxScreenCapture TestLinuxScreenCapture.cpp
    "${CAPTURE_SOURCE_DIR}/PipeWireScreenVideoSource.cpp"
    "${CAPTURE_SOURCE_DIR}/PortalScreenCast.cpp"
    "${CAPTURE_SOURCE_DIR}/VideoSource.cpp")
set_target_properties(TestLinuxScreenCapture PROPERTIES AUTOMOC ON)
target_include_directories(TestLinuxScreenCapture PRIVATE "${CAPTURE_SOURCE_DIR}")
target_link_libraries(TestLinuxScreenCapture PRIVATE shared Qt6::Gui Qt6::DBus Qt6::Test PkgConfig::CAPTURE_PIPEWIRE)
find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
add_test(NAME TestLinuxScreenCapture COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:TestLinuxScreenCapture>)
set_tests_properties(TestLinuxScreenCapture PROPERTIES TIMEOUT 45)
