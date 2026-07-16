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

# LeiaSR autostereoscopic weaving (Windows + D3D12 backend only).
# Vendored at extern/leiasr; overridable via LEIASR_SDKROOT (env var wins).
# Re-derived every configure so a stale cache (e.g. after moving the submodule)
# doesn't strand us pointing at a path that no longer exists.
set(_leiasr_env "$ENV{LEIASR_SDKROOT}")
set(_leiasr_vendored "${CMAKE_CURRENT_SOURCE_DIR}/extern/leiasr")
set(_leiasr_resolved "")
if (WIN32)
  if (_leiasr_env AND EXISTS "${_leiasr_env}/lib/cmake")
    set(_leiasr_resolved "${_leiasr_env}")
  elseif (EXISTS "${_leiasr_vendored}/lib/cmake")
    set(_leiasr_resolved "${_leiasr_vendored}")
  endif ()
endif ()
set(LEIASR_SDKROOT "${_leiasr_resolved}" CACHE PATH "Path to LeiaSR / Simulated Reality SDK root" FORCE)
set(AURORA_ENABLE_LEIASR OFF)
if (LEIASR_SDKROOT)
  set(AURORA_ENABLE_LEIASR ON)
  message(STATUS "aurora: LeiaSR enabled (${LEIASR_SDKROOT})")
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
