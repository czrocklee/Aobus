cmake_minimum_required(VERSION 3.30)

project(rockstudio VERSION 0.1.0)
set (CMAKE_CXX_STANDARD 23)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/rs")
configure_file(cmake/AppVersion.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/rs/AppVersion.h" @ONLY)

# Enable testing
enable_testing()

# Strict compiler warnings (disable -Werror when using sanitizers, also disable for third-party headers)
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
  add_compile_options(-Wall -Wextra -Wpedantic)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-error)
endif()

# GSL-lite configuration: Debug throws exceptions, non-Debug disables contract checks
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(gsl_CONFIG_CONTRACT_VIOLATION_ASSERTS=1)
else()
  add_compile_definitions(gsl_CONFIG_CONTRACT_CHECKING_OFF=1)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)
pkg_check_modules(CATCH2 REQUIRED catch2)

find_package(Boost REQUIRED COMPONENTS headers)
find_package(CLI11 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp REQUIRED)

find_program(GLIB_COMPILE_RESOURCES_EXECUTABLE glib-compile-resources REQUIRED)
find_program(GPERF_EXECUTABLE gperf REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Gperf.cmake)

add_library(GTK4 INTERFACE)
target_include_directories(GTK4 INTERFACE ${GTK4_INCLUDE_DIRS})
target_link_libraries(GTK4 INTERFACE ${GTK4_LIBRARIES})
target_compile_options(GTK4 INTERFACE ${GTK4_CFLAGS_OTHER})

add_library(GTKMM INTERFACE)
target_include_directories(GTKMM INTERFACE ${GTKMM_INCLUDE_DIRS})
target_link_libraries(GTKMM INTERFACE ${GTKMM_LIBRARIES})
target_compile_options(GTKMM INTERFACE ${GTKMM_CFLAGS_OTHER})

add_library(Catch2 INTERFACE)
target_include_directories(Catch2 INTERFACE ${CATCH2_CFLAGS})
target_link_libraries(Catch2 INTERFACE ${CATCH2_LDFLAGS})

# FFmpeg via pkg-config (optional - playback feature)
pkg_check_modules(AVFORMAT libavformat)
pkg_check_modules(AVCODEC libavcodec)
pkg_check_modules(AVUTIL libavutil)
pkg_check_modules(SWRESAMPLE libswresample)
if(NOT AVFORMAT_FOUND)
  pkg_check_modules(AVFORMAT avformat)
  pkg_check_modules(AVCODEC avcodec)
  pkg_check_modules(AVUTIL avutil)
  pkg_check_modules(SWRESAMPLE swresample)
endif()
if(AVFORMAT_FOUND AND AVCODEC_FOUND AND AVUTIL_FOUND AND SWRESAMPLE_FOUND)
  set(FFmpeg_FOUND TRUE)
  add_library(FFmpeg INTERFACE)
  target_include_directories(FFmpeg INTERFACE ${AVFORMAT_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${SWRESAMPLE_INCLUDE_DIRS})
  target_link_libraries(FFmpeg INTERFACE ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${SWRESAMPLE_LIBRARIES})
  message(STATUS "FFmpeg found: playback support enabled")
else()
  set(FFmpeg_FOUND FALSE)
  message(STATUS "FFmpeg not found: playback support disabled")
endif()

# PipeWire via pkg-config (optional - audio backend)
pkg_check_modules(PIPEWIRE libpipewire-0.3)
if(PIPEWIRE_FOUND)
  add_library(PipeWire INTERFACE)
  target_include_directories(PipeWire SYSTEM INTERFACE ${PIPEWIRE_INCLUDE_DIRS})
  target_link_libraries(PipeWire INTERFACE ${PIPEWIRE_LIBRARIES})
  message(STATUS "PipeWire found: audio backend enabled")
endif()

# ALSA via pkg-config (optional - audio backend)
pkg_check_modules(ALSA alsa)
if(ALSA_FOUND)
  add_library(ALSA INTERFACE)
  target_include_directories(ALSA INTERFACE ${ALSA_INCLUDE_DIRS})
  target_link_libraries(ALSA INTERFACE ${ALSA_LIBRARIES})
  message(STATUS "ALSA found: ALSA exclusive backend enabled")
endif()

# clang-tidy setup (manual application to targets)
#find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
unset(CLANG_TIDY_EXECUTABLE)

if(CLANG_TIDY_EXECUTABLE)
  # Set up GCC include paths for clang-tidy (required for GCC)
  set(CLANG_TIDY_EXTRA_ARGS)
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -E -x c++ - -v
    INPUT_FILE /dev/null
    OUTPUT_QUIET
    ERROR_VARIABLE CXX_INCLUDE_OUTPUT
    ERROR_STRIP_TRAILING_WHITESPACE
  )
  string(REGEX MATCHALL "[\n\r][ ]+/[^\n\r]+" CXX_INCLUDE_DIRS "\n${CXX_INCLUDE_OUTPUT}")
  foreach(CXX_INCLUDE_DIR IN LISTS CXX_INCLUDE_DIRS)
    string(STRIP "${CXX_INCLUDE_DIR}" CXX_INCLUDE_DIR)
    if(IS_DIRECTORY "${CXX_INCLUDE_DIR}")
      list(APPEND CLANG_TIDY_EXTRA_ARGS --extra-arg-before=-isystem${CXX_INCLUDE_DIR})
    endif()
  endforeach()

  # clang-tidy checks for strict targets (lib, tool)
  set(CLANG_TIDY_STRICT_CHECKS
    "-checks=-*,bugprone-*,performance-*,cppcoreguidelines-*,-cppcoreguidelines-avoid-const-or-ref-data-members,modernize-!-trailing-return,readability-*,portability-*,-clang-diagnostic-*,-bugprone-easily-swappable-parameters,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-readability-convert-member-functions-to-static,-readability-static-definition-in-anonymous-namespace"
  )

  # clang-tidy checks for relaxed test targets (minimal checks)
  set(CLANG_TIDY_RELAXED_CHECKS
    "-checks=-*,performance-avoid-endl"
  )

  # Strict header filter (exclude test/)
  set(CLANG_TIDY_STRICT_FILTER "'/home/rocklee/dev/RockStudio/(lib|tool|app)/.*'")
  set(CLANG_TIDY_RELAXED_FILTER "'/home/rocklee/dev/RockStudio/test/.*'")

  message(STATUS "clang-tidy found: ${CLANG_TIDY_EXECUTABLE}")
  message(STATUS "clang-tidy strict checks for lib/tool, relaxed for test/")
else()
  message(STATUS "clang-tidy not found - install it for linting")
endif()

add_gperf_header(
  FLAC_VORBIS_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/tag/flac/VorbisCommentDispatch.gperf"
  "tag/flac/VorbisCommentDispatch.h"
)
add_gperf_header(
  MP4_ATOM_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/tag/mp4/AtomDispatch.gperf"
  "tag/mp4/AtomDispatch.h"
)
add_gperf_header(
  MPEG_FRAME_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/tag/mpeg/id3v2/FrameDispatch.gperf"
  "tag/mpeg/id3v2/FrameDispatch.h"
)
add_gperf_header(
  EXPR_property_dispatch_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/expr/PropertyDispatch.gperf"
  "expr/PropertyDispatch.h"
)
add_gperf_header(
  EXPR_metadata_dispatch_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/expr/MetadataDispatch.gperf"
  "expr/MetadataDispatch.h"
)
add_gperf_header(
  EXPR_UNIT_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/expr/UnitDispatch.gperf"
  "expr/UnitDispatch.h"
)
add_custom_target(
  tag_dispatch_headers
  DEPENDS
    "${FLAC_VORBIS_DISPATCH_HEADER}"
    "${MP4_ATOM_DISPATCH_HEADER}"
    "${MPEG_FRAME_DISPATCH_HEADER}"
    "${EXPR_property_dispatch_HEADER}"
    "${EXPR_metadata_dispatch_HEADER}"
    "${EXPR_UNIT_DISPATCH_HEADER}"
)

# Module targets
add_subdirectory(lib)
add_subdirectory(tool)
add_subdirectory(test)
add_subdirectory(app)
