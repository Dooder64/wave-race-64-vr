# Phase 4a: a standalone OpenXR test, built apart from the game so that a
# headset or runtime problem cannot hide behind anything of the port's.
# Built by tools/build_xr_test.ps1.
cmake_minimum_required(VERSION 3.20)
project(wr64_xr_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The OpenXR loader, as a static library (its Windows default), and none of
# the SDK's layers, tests or samples.
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/OpenXR-SDK ${CMAKE_CURRENT_BINARY_DIR}/openxr EXCLUDE_FROM_ALL)

add_executable(wr64_xr_test xr_test.cpp)
target_link_libraries(wr64_xr_test PRIVATE OpenXR::openxr_loader d3d12 dxgi)
