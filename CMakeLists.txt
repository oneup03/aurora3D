cmake_minimum_required(VERSION 3.25)
project(aurora LANGUAGES C CXX)
if (APPLE)
    enable_language(OBJC)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AURORA_ENABLE_GX "Enable GX implementation and WebGPU renderer" ON)
option(AURORA_ENABLE_DVD "Enable DVD implementation backed by nod" OFF)  # TODO: need to fix tests
option(AURORA_ENABLE_CARD "Enable CARD implementation based on kabufuda" ON)
option(AURORA_ENABLE_RMLUI "Enable HTML/CSS Based UI Library for use in end-user UI development." OFF)
option(AURORA_CACHE_USE_ZSTD "Compress WebGPU cache entries with zstd" ON)

# LeiaSR autostereoscopic weaving (Windows x64 + D3D12 backend only).
#
# The SR SDK is not on any package index, so it is vendored through the
# bo3b/SR-lib submodule (api_expansion branch) at extern/SR-lib. Its CMakeLists
# locates the nested per-architecture SDK tree, exposes SRLib::SDK (headers +
# import libs) and SRLib::SR (that, plus SR.cpp's SRInterface* wrapper), and
# provides srlib_apply_delayload() carrying the complete /DELAYLOAD list. Our
# lib/webgpu/leiasr.cpp drives the wrapper, so aurora_core links SRLib::SR.
#
# Deliberately not find_package(simulatedreality): the SDK's own CMake configs
# declare their targets SHARED IMPORTED and CMake then demands the runtime
# bin/*.dll sit next to each .lib. SR-lib ships import libraries only -- the
# DLLs belong to the user's system-wide SR Platform install and must not be
# redistributed -- so find_package fails with a misleading "The installation
# package was faulty".
#
# SRLIB_FOUND comes back FALSE on non-Windows and when the (private) submodule
# was never checked out, so a clone without SR-lib access still configures and
# builds; LeiaSR just isn't offered and the game falls back to the other stereo
# modes.
#
# The SR SDK ships x64 (and x86) Windows import libraries only -- there is no
# ARM64 build of the SR runtime -- so LeiaSR must be excluded on Windows-on-ARM
# targets. Detect the TARGET arch (not the host): MSVC populates
# CMAKE_CXX_COMPILER_ARCHITECTURE_ID (ARM / ARM64 / ARM64EC / X86 / x64), and
# CMAKE_SYSTEM_PROCESSOR (ARM64 / AMD64 / ...) is the fallback for other
# toolchains.
set(_leiasr_is_arm FALSE)
if (CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "[Aa][Rr][Mm]" OR
    CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]")
  set(_leiasr_is_arm TRUE)
endif ()

set(SRLIB_FOUND FALSE)
if (WIN32 AND NOT _leiasr_is_arm AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/SR-lib/CMakeLists.txt")
  add_subdirectory(extern/SR-lib)
endif ()

# Cached (not a plain variable) so consumers that add_subdirectory() aurora --
# e.g. dusklight, which needs to know whether to call srlib_apply_delayload() on
# its executable -- can see it from their own scope. Re-derived every configure
# so a stale cache from a build where the submodule was missing doesn't stick.
set(AURORA_ENABLE_LEIASR ${SRLIB_FOUND} CACHE INTERNAL "LeiaSR autostereoscopic output is available")
if (AURORA_ENABLE_LEIASR)
  message(STATUS "aurora: LeiaSR enabled (${SRLIB_SDKROOT})")
endif ()

# Dependency versions
include(cmake/AuroraDependencyVersions.cmake)

# Platform-specific defaults
set(_default_provider "auto")
if (WIN32)
  set(_default_linkage "shared")
else ()
  set(_default_linkage "static")
endif ()

# Dawn/WebGPU (if AURORA_ENABLE_GX)
set(AURORA_DAWN_PROVIDER "${_default_provider}" CACHE STRING
  "How to provide Dawn: auto, vendor (build from source), system (find_package/imported), package (download prebuilt)")
set_property(CACHE AURORA_DAWN_PROVIDER PROPERTY STRINGS auto vendor system package)
set(AURORA_DAWN_LINKAGE "${_default_linkage}" CACHE STRING "Dawn linkage type preference")
set_property(CACHE AURORA_DAWN_LINKAGE PROPERTY STRINGS shared static)

# SDL3
set(AURORA_SDL3_PROVIDER "${_default_provider}" CACHE STRING
  "How to provide SDL3: auto, vendor (build from source), system (find_package/imported), or package (download prebuilt)")
set_property(CACHE AURORA_SDL3_PROVIDER PROPERTY STRINGS auto vendor system package)
set(AURORA_SDL3_LINKAGE "${_default_linkage}" CACHE STRING "SDL3 linkage type preference")
set_property(CACHE AURORA_SDL3_LINKAGE PROPERTY STRINGS shared static)

# nod (if AURORA_ENABLE_DVD)
set(AURORA_NOD_PROVIDER "${_default_provider}" CACHE STRING
  "How to provide nod: auto, vendor (build from source), system (find_package/imported), package (download prebuilt)")
set_property(CACHE AURORA_NOD_PROVIDER PROPERTY STRINGS auto vendor system package)
set(AURORA_NOD_LINKAGE "${_default_linkage}" CACHE STRING "nod linkage type preference")
set_property(CACHE AURORA_NOD_LINKAGE PROPERTY STRINGS shared static)

add_subdirectory(extern)

include(cmake/aurora_core.cmake)
include(cmake/aurora_os.cmake)
if (AURORA_ENABLE_GX)
  include(cmake/aurora_gx.cmake)
  include(cmake/aurora_gd.cmake)
endif ()
include(cmake/aurora_pad.cmake)
include(cmake/aurora_ms.cmake)
include(cmake/aurora_si.cmake)
include(cmake/aurora_main.cmake)
include(cmake/aurora_mtx.cmake)
include(cmake/aurora_vi.cmake)
if(AURORA_ENABLE_CARD)
  include(cmake/aurora_card.cmake)
endif()
if (AURORA_ENABLE_DVD)
  include(cmake/aurora_dvd.cmake)
endif ()

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_subdirectory(examples)
endif ()

if (NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_subdirectory(tests)
endif ()
