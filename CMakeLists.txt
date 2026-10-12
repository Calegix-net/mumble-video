# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.


cmake_minimum_required(VERSION 3.23...4.2)

set(BUILD_NUMBER CACHE STRING "The build number of the current build. Will be used in Mumble's version to make sure newer builds upgrade older installations properly.")
set(BUILD_RELEASE_DATE CACHE STRING "The release date to be used in the generated appstream metadata.")

if ("${BUILD_NUMBER}" STREQUAL "")
	if(packaging)
		message(FATAL_ERROR "Tried to create a Mumble package, without specifying BUILD_NUMBER!")
	else()
		set(BUILD_NUMBER "0")
	endif()
endif()

project(mumble
	VERSION "1.7.${BUILD_NUMBER}"
	DESCRIPTION "Open source, low-latency, high quality voice chat."
	HOMEPAGE_URL "https://www.mumble.info"
	LANGUAGES "C" "CXX"
)

# Two versions, deliberately different.
#
# The project version above is the upstream Mumble release this fork tracks. It is the protocol version
# (Version::get(), sent as version_v1/version_v2), and upstream peers decide which features to use by
# comparing it, so it moves only when the protocol does. See src/CMakeLists.txt.
#
# The release version is what this fork is released as, e.g. 1.9.1. It is only ever shown: the About
# dialog, --version, the server log, and the release string clients and servers exchange for display.
# CI passes it in (scripts/installer-version.sh --release). Left empty, it is worked out from the
# newest vX.Y.Z tag: that release on the tagged commit, "<release>-dev+<commit>" after it - the same
# shape CI gives development builds. With no tags to go by (a shallow clone, the flatpak sandbox, a
# source archive) it is "unknown-dev" rather than a release number it cannot vouch for: falling back to
# the project version would say 1.7.0, which is exactly the confusion this is here to end.
# It is worked out at configure time; reconfigure to pick up a new commit.
set(release-version "" CACHE STRING "The version this build reports itself as, e.g. 1.9.1 or 1.9.1-dev+0123abcde. Worked out from git when empty.")

set(MUMBLE_RELEASE_VERSION "${release-version}")
if(MUMBLE_RELEASE_VERSION STREQUAL "")
	set(MUMBLE_RELEASE_VERSION "unknown-dev")
	find_package(Git QUIET)
	if(GIT_FOUND)
		# --exclude keeps a pre-release tag such as v2.0.0-rc1 from shadowing the release before it.
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" describe --tags --long --abbrev=9
				--match "v[0-9]*.[0-9]*.[0-9]*" --exclude "v*-*"
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			OUTPUT_VARIABLE GIT_DESCRIBE
			RESULT_VARIABLE GIT_DESCRIBE_RESULT
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
		)
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" rev-parse --short=9 HEAD
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			OUTPUT_VARIABLE GIT_COMMIT
			RESULT_VARIABLE GIT_COMMIT_RESULT
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
		)
		if(GIT_DESCRIBE_RESULT EQUAL 0 AND GIT_DESCRIBE MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)-([0-9]+)-g([0-9a-f]+)$")
			if(CMAKE_MATCH_2 EQUAL 0)
				set(MUMBLE_RELEASE_VERSION "${CMAKE_MATCH_1}")
			else()
				set(MUMBLE_RELEASE_VERSION "${CMAKE_MATCH_1}-dev+${CMAKE_MATCH_3}")
			endif()
		elseif(GIT_COMMIT_RESULT EQUAL 0 AND GIT_COMMIT MATCHES "^[0-9a-f]+$")
			set(MUMBLE_RELEASE_VERSION "unknown-dev+${GIT_COMMIT}")
		endif()
	endif()
endif()

# It ends up inside a C string literal and a Windows resource script, so nothing that would need
# escaping in either.
if(NOT MUMBLE_RELEASE_VERSION MATCHES "^[0-9A-Za-z][0-9A-Za-z.+_-]*$")
	message(FATAL_ERROR "release-version \"${MUMBLE_RELEASE_VERSION}\" may only contain letters, digits and . + _ -")
endif()

set(3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty")
set(PLUGINS_DIR "${CMAKE_SOURCE_DIR}/plugins")

list(APPEND CMAKE_MODULE_PATH
	"${CMAKE_SOURCE_DIR}/cmake"
	"${CMAKE_SOURCE_DIR}/cmake/FindModules"
	"${3RDPARTY_DIR}/cmake-compiler-flags"
)

if(NOT DEFINED CMAKE_CXX_STANDARD OR CMAKE_CXX_STANDARD LESS 20)
	set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15)


include(pkg-utils)
include(project-utils)
include(TargetArch)
include(CheckIPOSupported)

find_package(Python 3 COMPONENTS Interpreter REQUIRED)


check_ipo_supported(RESULT LTO_DEFAULT)

if(MINGW)
	# GCC's LTO miscompiles this client for Windows. The build succeeds, the binary starts and prints
	# its version banner, and then faults on execute access to an address that is not code - the
	# signature of a call through a pointer LTO got wrong. Verified by building the same tree with LTO
	# off, which runs. check_ipo_supported() only answers whether the toolchain accepts -flto, not
	# whether the result works, so the default has to be overridden here.
	set(LTO_DEFAULT OFF)
endif()



option(optimize "Build a heavily optimized version, specific to the machine it's being compiled on." OFF)
option(static "Build static binaries." OFF)
option(symbols "Build binaries in a way that allows easier debugging." OFF)
option(warnings-as-errors "All warnings are treated as errors." ON)
option(use-timestamps "Allow using compile-time timestamps" ON)

option(client "Build the client (Mumble)" ON)
option(server "Build the server (Murmur)" ON)
option(packaging "Build package." OFF)
option(tests "Build tests." ${packaging})
option(plugins "Build plugins." ON)

if (WIN32)
	option(skip-msi-rebuild "Prevent rebuilding installer MSI files from source. Used to be able to include signed MSI files in the bundle." OFF)
endif()

option(debug-dependency-search "Prints extended information during the search for the needed dependencies" OFF)

if(NOT CMAKE_BUILD_TYPE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY VALUE "Release")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(
        "-DDEBUG"
        "-DSNAPSHOT_BUILD"
    )
endif()

if (NOT DEFINED CMAKE_CXX_SCAN_FOR_MODULES)
	# For now, we don't use C++ modules, so we can save ourselves the overhead
	# of scanning every source file (and excluding them from unity buils)
	set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

option(lto "Enables link-time optimizations for release builds" ${LTO_DEFAULT})

# Screen capture on Linux needs PipeWire and QtDBus. Defaulted on where those are the norm, but
# optional so a build on a system without them is a flag away rather than a patch away.
if(UNIX AND NOT APPLE)
	option(screen-share-pipewire "Build Linux screen sharing (XDG desktop portal + PipeWire)." ON)
	option(screen-share-x11 "Build the X11 screen-capture fallback, for desktops with no ScreenCast portal." ON)
endif()

option(bundled-utfcpp "Use the bundled utf8cpp version instead of looking for one on the system" ON)

include(compiler)
include(os)

target_architecture(MUMBLE_TARGET_ARCH)
string(TOLOWER "${MUMBLE_TARGET_ARCH}" MUMBLE_TARGET_ARCH)

# On macOS universal builds, CMAKE_OSX_ARCHITECTURES (and thus target_architecture())
# can result in a CMake list (e.g. "arm64;x86_64"). Using that list directly in
# compile definitions breaks quoting for some generators (notably Unix Makefiles).
# For those cases, expose a stable, single-string arch identifier.
if(APPLE AND MUMBLE_TARGET_ARCH MATCHES ";")
	set(MUMBLE_TARGET_ARCH "universal")
endif()

if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "OpenBSD")
	option(overlay "Build overlay." ${client})
	if(64_BIT AND NOT MUMBLE_TARGET_ARCH STREQUAL "arm64")
		option(overlay-xcompile "Build 32 bit overlay library, necessary for the overlay to work with 32 bit processes." ${client})
	endif()
endif()

message(STATUS "##################################################")
message(STATUS "Mumble version:              ${PROJECT_VERSION} (protocol)")
message(STATUS "Mumble release:              ${MUMBLE_RELEASE_VERSION}")
message(STATUS "Architecture:                ${MUMBLE_TARGET_ARCH}")
if(NOT IS_MULTI_CONFIG)
    message(STATUS "Build type:                  ${CMAKE_BUILD_TYPE}")
else()
    message(STATUS "Using multi-config generator that will determine build type on-the-fly")
endif()
message(STATUS "Using LTO in release builds: ${lto}")
message(STATUS "##################################################")

include(install-paths)

# We have to check for BUILD_TESTING before including CTest as CTest defines this variable
if(DEFINED BUILD_TESTING AND NOT BUILD_TESTING_CHECKED)
	message(WARNING "Use of option \"BUILD_TESTING\" is deprecated. Use \"tests\" instead.")

	if(NOT tests)
		# Allow deprecated option to enable tests if they had been disabled otherwise
		set(tests "${BUILD_TESTING}")
	endif()
endif()

set(BUILD_TESTING_CHECKED ON INTERNAL CACHE BOOL "Persistent helper variable" FORCE)

if(tests)
	include(CTest)
endif()

if (WIN32)
	set(MUMBLE_TARGET_OS "windows")
elseif (APPLE)
	set(MUMBLE_TARGET_OS "macos")
elseif (UNIX)
	set(MUMBLE_TARGET_OS "linux")
else()
	message(FATAL_ERROR "Unable to determine target OS")
endif()

# Get compilation year
if (use-timestamps)
	string(TIMESTAMP MUMBLE_BUILD_YEAR "%Y")
else()
	set(MUMBLE_BUILD_YEAR "now")
endif()

# Make the build year accessible as a macro
add_compile_definitions(MUMBLE_BUILD_YEAR=${MUMBLE_BUILD_YEAR})

# Make sure that math constants are always defined
add_compile_definitions(_USE_MATH_DEFINES)


# Provide the information about the target architecture to all Mumble source files in form of a macro
add_compile_definitions(MUMBLE_TARGET_ARCH="${MUMBLE_TARGET_ARCH}")

# Also provide information about the target OS
add_compile_definitions(MUMBLE_TARGET_OS="${MUMBLE_TARGET_OS}")


set(CMAKE_UNITY_BUILD_BATCH_SIZE 40)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${lto})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

if(bundled-utfcpp)
	add_subdirectory(${3RDPARTY_DIR}/utfcpp)
else()
	find_pkg("utf8cpp" REQUIRED)
	add_library(utfcpp ALIAS utf8cpp::utf8cpp)
endif()

if(g15 AND WIN32)
	add_subdirectory("helpers/g15helper")
endif()

if(APPLE AND MUMBLE_TARGET_ARCH MATCHES "arm.*")
	# mach_override doesn't support ARM
	# https://github.com/rentzsch/mach_override/issues/6
	set(overlay OFF CACHE BOOL "" FORCE)
	message(STATUS "Disabling the overlay on ARM macOS")
endif()

if(overlay)
        if(WIN32)
                add_subdirectory(overlay)
        else()
                if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
                        add_subdirectory(overlay_gl)
                endif()

                if(APPLE)
                        add_subdirectory(macx/osax)
                endif()
        endif()
endif()

if(plugins AND client)
	add_subdirectory(plugins)
endif()

if (packaging AND WIN32)
	set(VC_REDIST_PATH "${PROJECT_BINARY_DIR}/installer/VC_redist.x64.exe")
	file(
		DOWNLOAD "https://aka.ms/vs/17/release/vc_redist.x64.exe" "${VC_REDIST_PATH}"
		STATUS VC_REDIST_DOWNLOAD_STATUS
	)

	list(GET VC_REDIST_DOWNLOAD_STATUS 0 STATUS_CODE)
	if (NOT (STATUS_CODE EQUAL "0"))
		list(GET VC_REDIST_DOWNLOAD_STATUS 1 ERROR_MSG)
		message(FATAL_ERROR "Failed at downloading the VC redist installer: ${ERROR_MSG}")
	endif()

	execute_process(COMMAND
		powershell -Command "
			$exe = (Get-Item -path '${VC_REDIST_PATH}')
			$exe.VersionInfo.ProductVersion"
		OUTPUT_VARIABLE VC_REDIST_VERSION
		RESULT_VARIABLE VC_REDIST_VERSION_STATUS
	)

	if (NOT (VC_REDIST_VERSION_STATUS EQUAL "0"))
		message(FATAL_ERROR "Failed at extracting the VC redist version")
	endif()

	string(STRIP "${VC_REDIST_VERSION}" VC_REDIST_VERSION)

	message(STATUS "Using VC redist >= ${VC_REDIST_VERSION} as dependency")
endif()

if(client OR server)
	add_subdirectory(src)
endif()

add_subdirectory(auxiliary_files)

if(packaging)
	file(COPY
		${CMAKE_SOURCE_DIR}/installer/gpl.txt
		${CMAKE_SOURCE_DIR}/installer/lgpl.txt
		${CMAKE_SOURCE_DIR}/installer/Mumble.rtf
		${CMAKE_SOURCE_DIR}/installer/portaudio.txt
		${CMAKE_SOURCE_DIR}/installer/qt.txt
		${CMAKE_SOURCE_DIR}/installer/speex.txt
		DESTINATION
			${CMAKE_BINARY_DIR}/licenses
	)
	if(WIN32)
		file(COPY
			${CMAKE_SOURCE_DIR}/installer/bannrbmp.bmp
			${CMAKE_SOURCE_DIR}/installer/dlgbmp.bmp
			${CMAKE_SOURCE_DIR}/installer/Theme.xml
			DESTINATION
				${CMAKE_BINARY_DIR}/installer
		)

		file(COPY
			${CMAKE_SOURCE_DIR}/icons/mumble.ico
			${CMAKE_SOURCE_DIR}/icons/murmur.ico
			DESTINATION
				${CMAKE_BINARY_DIR}/installer/icons
		)
	endif()
endif()
