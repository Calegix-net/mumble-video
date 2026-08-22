# Install script for directory: /src/3rdparty/tracy

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

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64" TYPE STATIC_LIBRARY FILES "/build/src/tracy/libTracyClient.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/tracy" TYPE FILE FILES
    "/src/3rdparty/tracy/public/tracy/TracyC.h"
    "/src/3rdparty/tracy/public/tracy/Tracy.hpp"
    "/src/3rdparty/tracy/public/tracy/TracyD3D11.hpp"
    "/src/3rdparty/tracy/public/tracy/TracyD3D12.hpp"
    "/src/3rdparty/tracy/public/tracy/TracyLua.hpp"
    "/src/3rdparty/tracy/public/tracy/TracyOpenCL.hpp"
    "/src/3rdparty/tracy/public/tracy/TracyOpenGL.hpp"
    "/src/3rdparty/tracy/public/tracy/TracyVulkan.hpp"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/client" TYPE FILE FILES
    "/src/3rdparty/tracy/public/client/tracy_concurrentqueue.h"
    "/src/3rdparty/tracy/public/client/tracy_rpmalloc.hpp"
    "/src/3rdparty/tracy/public/client/tracy_SPSCQueue.h"
    "/src/3rdparty/tracy/public/client/TracyArmCpuTable.hpp"
    "/src/3rdparty/tracy/public/client/TracyCallstack.h"
    "/src/3rdparty/tracy/public/client/TracyCallstack.hpp"
    "/src/3rdparty/tracy/public/client/TracyDebug.hpp"
    "/src/3rdparty/tracy/public/client/TracyDxt1.hpp"
    "/src/3rdparty/tracy/public/client/TracyFastVector.hpp"
    "/src/3rdparty/tracy/public/client/TracyLock.hpp"
    "/src/3rdparty/tracy/public/client/TracyProfiler.hpp"
    "/src/3rdparty/tracy/public/client/TracyRingBuffer.hpp"
    "/src/3rdparty/tracy/public/client/TracyScoped.hpp"
    "/src/3rdparty/tracy/public/client/TracyStringHelpers.hpp"
    "/src/3rdparty/tracy/public/client/TracySysTime.hpp"
    "/src/3rdparty/tracy/public/client/TracySysTrace.hpp"
    "/src/3rdparty/tracy/public/client/TracyThread.hpp"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/common" TYPE FILE FILES
    "/src/3rdparty/tracy/public/common/tracy_lz4.hpp"
    "/src/3rdparty/tracy/public/common/tracy_lz4hc.hpp"
    "/src/3rdparty/tracy/public/common/TracyAlign.hpp"
    "/src/3rdparty/tracy/public/common/TracyAlloc.hpp"
    "/src/3rdparty/tracy/public/common/TracyApi.h"
    "/src/3rdparty/tracy/public/common/TracyColor.hpp"
    "/src/3rdparty/tracy/public/common/TracyForceInline.hpp"
    "/src/3rdparty/tracy/public/common/TracyMutex.hpp"
    "/src/3rdparty/tracy/public/common/TracyProtocol.hpp"
    "/src/3rdparty/tracy/public/common/TracyQueue.hpp"
    "/src/3rdparty/tracy/public/common/TracySocket.hpp"
    "/src/3rdparty/tracy/public/common/TracyStackFrames.hpp"
    "/src/3rdparty/tracy/public/common/TracySystem.hpp"
    "/src/3rdparty/tracy/public/common/TracyUwp.hpp"
    "/src/3rdparty/tracy/public/common/TracyYield.hpp"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/Tracy/TracyConfig.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/Tracy/TracyConfig.cmake"
         "/build/src/tracy/CMakeFiles/Export/7430802ac276f58e70c46cf34d169c6f/TracyConfig.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/Tracy/TracyConfig-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/Tracy/TracyConfig.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/Tracy" TYPE FILE FILES "/build/src/tracy/CMakeFiles/Export/7430802ac276f58e70c46cf34d169c6f/TracyConfig.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/Tracy" TYPE FILE FILES "/build/src/tracy/CMakeFiles/Export/7430802ac276f58e70c46cf34d169c6f/TracyConfig-release.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/build/src/tracy/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
