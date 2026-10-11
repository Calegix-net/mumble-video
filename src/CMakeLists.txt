# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

option(benchmarks "Build benchmarks" OFF)

option(qssldiffiehellmanparameters "Build support for custom Diffie-Hellman parameters." ON)

option(zeroconf "Build support for zeroconf (mDNS/DNS-SD)." ON)

option(tracy "Enable the tracy profiler." OFF)

option(bundled-json "Build the included version of nlohmann_json instead of looking for one on the system" ON)
option(bundled-spdlog "Use the bundled spdlog version instead of looking for one on the system" ON)
option(bundled-cli11 "Use the bundled CLI11 version instead of looking for one on the system" ON)

find_pkg(Qt6
	VERSION 6.2
	COMPONENTS
		Core
		Network
		Xml
	REQUIRED
)

find_pkg(OpenSSL
	COMPONENTS
		Crypto
		SSL
	REQUIRED
)

find_pkg(Protobuf REQUIRED)

add_compile_options(${MUMBLE_COMPILER_FLAGS})

# The Qt-free protocol core, including the generated Protobuf code.
add_subdirectory(mumble-core)

add_library(shared STATIC)

set_target_properties(shared
	PROPERTIES
		AUTOMOC ON
)

set(SHARED_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(SHARED_SOURCES
	"Ban.cpp"
	"CoreLogBridge.cpp"
	"EnvUtils.cpp"
	"ExceptionUtils.cpp"
	"FFDHE.cpp"
	"HostAddress.cpp"
	"HTMLFilter.cpp"
	"License.cpp"
	"LogEmitter.cpp"
	"Logger.cpp"
	"OSInfo.cpp"
	"PasswordGenerator.cpp"
	"PlatformCheck.cpp"
	"QtUtils.cpp"
	"ProcessResolver.cpp"
	"ProtoUtils.cpp"
	"SelfSignedCertificate.cpp"
	"ServerAddress.cpp"
	"ServerResolver.cpp"
	"ServerResolverRecord.cpp"
	"SSL.cpp"
	"SSLLocks.cpp"
	"UnresolvedServerAddress.cpp"
	"Version.cpp"

	"crypto/CryptographicHash.cpp"
)

set(SHARED_HEADERS
	"Ban.h"
	"CoreLogBridge.h"
	"EnvUtils.h"
	"ExceptionUtils.h"
	"FFDHE.h"
	"HostAddress.h"
	"HTMLFilter.h"
	"License.h"
	"LogEmitter.h"
	"Logger.h"
	"Net.h"
	"OSInfo.h"
	"PasswordGenerator.h"
	"PlatformCheck.h"
	"ProcessResolver.h"
	"ProtoUtils.h"
	"SelfSignedCertificate.h"
	"ServerAddress.h"
	"ServerResolver.h"
	"ServerResolverRecord.h"
	"SSL.h"
	"SSLLocks.h"
	"UnresolvedServerAddress.h"
	"Version.h"

	"crypto/CryptographicHash.h"
)

add_custom_command(
	OUTPUT "FFDHETable.h"
	COMMAND Python::Interpreter
	ARGS ${CMAKE_SOURCE_DIR}/scripts/generate-ffdhe.py --output FFDHETable.h
	COMMENT "Generating FFDHETable.h"
)
list(APPEND SHARED_HEADERS "FFDHETable.h")

add_custom_command(
	OUTPUT "licenses.h"
	COMMAND Python::Interpreter
	ARGS "${CMAKE_SOURCE_DIR}/scripts/generate_license_header.py" --output "licenses.h"
	COMMENT "Generating licenses.h"
)
add_custom_target(generate_license_header
	DEPENDS "licenses.h"
)
add_dependencies(shared generate_license_header)
target_include_directories(shared PRIVATE "${CMAKE_BINARY_DIR}")

target_sources(shared
	PRIVATE
		${SHARED_HEADERS}
		${SHARED_SOURCES}
)

# MUMBLE_VERSION_MAJOR/MINOR/PATCH, set on mumble-core, make Version::get(), the protocol version.
# Upstream Mumble gates behaviour on it in both directions - Messages.cpp and MainWindow.cpp compare a
# peer's version against Version::fromComponents(1, x, y) throughout, and the UDP format switches to
# protobuf at 1.5.0 - so it has to stay the upstream release whose protocol this fork actually speaks.
# Raising it to the fork's release number would tell an upstream 1.8 or 1.9 peer that we implement
# whatever it gates on those.
#
# MUMBLE_RELEASE_STRING is the fork's own release (see release-version in the top-level CMakeLists.txt)
# and becomes Version::getRelease(): display only, never compared. It is private to this target because
# only Version.cpp uses it, so a new commit does not rebuild everything that includes Version.h.
# MUMBLE_VERSION is the project version as a bare token, kept for the macOS overlay installer, which
# checks it against the minimum Mumble version the overlay asks for.
target_compile_definitions(shared
	PRIVATE
		"MUMBLE_RELEASE_STRING=\"${MUMBLE_RELEASE_VERSION}\""
		"QT_RESTRICTED_CAST_FROM_ASCII"
)

target_include_directories(shared
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(shared
	PUBLIC
		mumble-core
		protobuf::libprotobuf
		OpenSSL::Crypto
		OpenSSL::SSL
		Qt6::Core
		Qt6::Network
		Qt6::Xml
)

if(WIN32)
	target_link_libraries(shared PUBLIC qwave.lib)
elseif(APPLE)
	find_library(LIB_CORESERVICES "CoreServices")
	target_link_libraries(shared PUBLIC ${LIB_CORESERVICES})
endif()

if(qssldiffiehellmanparameters)
	target_compile_definitions(shared PUBLIC "USE_QSSLDIFFIEHELLMANPARAMETERS")
endif()

# Note: We always include and link against Tracy but it is only enabled, if we set the TRACY_ENABLE cmake option
# to ON, before including the respective subdirectory
set(TRACY_ENABLE ${tracy} CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
# We force to build Tracy as a static library in order to not create a dependency on the respective .so file
# (can cause issues for packagers as that dependency is retained even though Tracy is disabled)
set(PREV_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${3RDPARTY_DIR}/tracy" "tracy" EXCLUDE_FROM_ALL)
disable_warnings_for_all_targets_in("${3RDPARTY_DIR}/tracy")
message(STATUS "Tracy: ${TRACY_ENABLE}")

# Restore whatever BUILD_SHARED_LIBS was set to before
set(BUILD_SHARED_LIBS ${PREV_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)

target_link_libraries(shared PUBLIC Tracy::TracyClient)

if(bundled-spdlog)
	set(SPDLOG_SYSTEM_INCLUDES ON)
	add_subdirectory("${3RDPARTY_DIR}/spdlog" "${CMAKE_CURRENT_BINARY_DIR}/spdlog" EXCLUDE_FROM_ALL)
else()
	find_pkg("spdlog" REQUIRED)
endif()
target_link_libraries(shared PUBLIC spdlog::spdlog)

if (bundled-cli11)
	add_subdirectory("${3RDPARTY_DIR}/CLI11" "${CMAKE_CURRENT_BINARY_DIR}/CLI11" EXCLUDE_FROM_ALL)
else()
	find_pkg("CLI11" 2 REQUIRED)
endif()

if(client)
	add_subdirectory(mumble)

	if(UNIX AND NOT APPLE)
		install(FILES "${CMAKE_SOURCE_DIR}/icons/mumble.svg" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps")
		install(FILES "${CMAKE_SOURCE_DIR}/icons/mumble_256x256.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps" RENAME "mumble.png")
	endif()
endif()

if(server)
	add_subdirectory(database)
	add_subdirectory(murmur)
endif()

if(tests)
	add_subdirectory(tests)
endif()

if(benchmarks)
	add_subdirectory(benchmarks)
endif()
