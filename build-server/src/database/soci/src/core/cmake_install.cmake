# Install script for directory: /src/3rdparty/soci/src/core

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "soci_development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64" TYPE STATIC_LIBRARY FILES "/build/lib/libsoci_core.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "soci_development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/soci" TYPE FILE FILES
    "/src/3rdparty/soci/include/soci/backend-loader.h"
    "/src/3rdparty/soci/include/soci/bind-values.h"
    "/src/3rdparty/soci/include/soci/blob-exchange.h"
    "/src/3rdparty/soci/include/soci/blob.h"
    "/src/3rdparty/soci/include/soci/boost-fusion.h"
    "/src/3rdparty/soci/include/soci/boost-gregorian-date.h"
    "/src/3rdparty/soci/include/soci/boost-optional.h"
    "/src/3rdparty/soci/include/soci/boost-tuple.h"
    "/src/3rdparty/soci/include/soci/callbacks.h"
    "/src/3rdparty/soci/include/soci/column-info.h"
    "/src/3rdparty/soci/include/soci/connection-parameters.h"
    "/src/3rdparty/soci/include/soci/connection-pool.h"
    "/src/3rdparty/soci/include/soci/error.h"
    "/src/3rdparty/soci/include/soci/exchange-traits.h"
    "/src/3rdparty/soci/include/soci/fixed-size-ints.h"
    "/src/3rdparty/soci/include/soci/into-type.h"
    "/src/3rdparty/soci/include/soci/into.h"
    "/src/3rdparty/soci/include/soci/is-detected.h"
    "/src/3rdparty/soci/include/soci/log-context.h"
    "/src/3rdparty/soci/include/soci/logger.h"
    "/src/3rdparty/soci/include/soci/noreturn.h"
    "/src/3rdparty/soci/include/soci/once-temp-type.h"
    "/src/3rdparty/soci/include/soci/prepare-temp-type.h"
    "/src/3rdparty/soci/include/soci/procedure.h"
    "/src/3rdparty/soci/include/soci/query_transformation.h"
    "/src/3rdparty/soci/include/soci/ref-counted-prepare-info.h"
    "/src/3rdparty/soci/include/soci/ref-counted-statement.h"
    "/src/3rdparty/soci/include/soci/row-exchange.h"
    "/src/3rdparty/soci/include/soci/row.h"
    "/src/3rdparty/soci/include/soci/rowid-exchange.h"
    "/src/3rdparty/soci/include/soci/rowid.h"
    "/src/3rdparty/soci/include/soci/rowset.h"
    "/src/3rdparty/soci/include/soci/session.h"
    "/src/3rdparty/soci/include/soci/soci-backend.h"
    "/src/3rdparty/soci/include/soci/soci-platform.h"
    "/src/3rdparty/soci/include/soci/soci-simple.h"
    "/src/3rdparty/soci/include/soci/soci-types.h"
    "/src/3rdparty/soci/include/soci/soci-unicode.h"
    "/src/3rdparty/soci/include/soci/soci.h"
    "/src/3rdparty/soci/include/soci/statement.h"
    "/src/3rdparty/soci/include/soci/std-optional.h"
    "/src/3rdparty/soci/include/soci/transaction.h"
    "/src/3rdparty/soci/include/soci/trivial-blob-backend.h"
    "/src/3rdparty/soci/include/soci/type-conversion-traits.h"
    "/src/3rdparty/soci/include/soci/type-conversion.h"
    "/src/3rdparty/soci/include/soci/type-holder.h"
    "/src/3rdparty/soci/include/soci/type-ptr.h"
    "/src/3rdparty/soci/include/soci/type-wrappers.h"
    "/src/3rdparty/soci/include/soci/use-type.h"
    "/src/3rdparty/soci/include/soci/use.h"
    "/src/3rdparty/soci/include/soci/values-exchange.h"
    "/src/3rdparty/soci/include/soci/values.h"
    "/src/3rdparty/soci/include/soci/version.h"
    "/build/include/soci/soci-config.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "soci_development" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/soci-4.2.0/SOCICoreTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/soci-4.2.0/SOCICoreTargets.cmake"
         "/build/src/database/soci/src/core/CMakeFiles/Export/a437084264a7cecea42092bcaf5bb2e1/SOCICoreTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/soci-4.2.0/SOCICoreTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/soci-4.2.0/SOCICoreTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/cmake/soci-4.2.0" TYPE FILE FILES "/build/src/database/soci/src/core/CMakeFiles/Export/a437084264a7cecea42092bcaf5bb2e1/SOCICoreTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/cmake/soci-4.2.0" TYPE FILE FILES "/build/src/database/soci/src/core/CMakeFiles/Export/a437084264a7cecea42092bcaf5bb2e1/SOCICoreTargets-release.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/build/src/database/soci/src/core/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
