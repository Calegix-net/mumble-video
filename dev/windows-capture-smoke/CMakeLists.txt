cmake_minimum_required(VERSION 3.22)
project(MumbleWindowsCaptureSmoke LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
get_filename_component(ROOT "../.." ABSOLUTE)
set(CLIENT "${ROOT}/src/mumble")
add_executable(WindowsCaptureSmoke main.cpp
 "${ROOT}/src/Timer.cpp"
 "${CLIENT}/VideoSource.cpp" "${CLIENT}/VideoSource.h"
 "${CLIENT}/WgcWindowVideoSource.cpp" "${CLIENT}/WgcWindowVideoSource.h"
 "${CLIENT}/DxgiDisplayVideoSource.cpp" "${CLIENT}/DxgiDisplayVideoSource.h"
 "${CLIENT}/WasapiProcessLoopbackSource.cpp" "${CLIENT}/WasapiProcessLoopbackSource.h"
 "${CLIENT}/AudioLoopbackSource.h")
target_include_directories(WindowsCaptureSmoke PRIVATE "${CLIENT}" "${ROOT}/src")
target_link_libraries(WindowsCaptureSmoke PRIVATE Qt6::Widgets d3d11 dxgi runtimeobject ole32 mmdevapi winmm)
