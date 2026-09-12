# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license.
find_package(PkgConfig REQUIRED)
pkg_check_modules(CAPTURE_X11 REQUIRED IMPORTED_TARGET x11 xext)
find_pkg(Qt6 COMPONENTS Gui REQUIRED)
set(CAPTURE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/mumble")
add_executable(TestX11ScreenCapture TestX11ScreenCapture.cpp
    "${CAPTURE_SOURCE_DIR}/X11ScreenVideoSource.cpp"
    "${CAPTURE_SOURCE_DIR}/VideoSource.cpp")
set_target_properties(TestX11ScreenCapture PROPERTIES AUTOMOC ON)
target_include_directories(TestX11ScreenCapture PRIVATE "${CAPTURE_SOURCE_DIR}")
target_link_libraries(TestX11ScreenCapture PRIVATE shared Qt6::Gui Qt6::Test PkgConfig::CAPTURE_X11)
# The cases that need a real X server skip themselves without one, so this is runnable anywhere; under
# Xvfb it exercises the same MIT-SHM path a desktop does.
add_test(NAME TestX11ScreenCapture COMMAND $<TARGET_FILE:TestX11ScreenCapture>)
set_tests_properties(TestX11ScreenCapture PROPERTIES TIMEOUT 45)
