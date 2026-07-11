# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2025 Aobus Contributors
#
# Compiler and linker configuration for the Aobus project.

# C++ Standard
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Aobus deliberately does not use C++ modules. Disabling CMake's automatic
# scanner keeps generated .modmap response files out of native compile commands,
# including commands consumed before a target has ever been built.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)
  # CMake has no MSVC mapping for C++26 yet; 23 selects /std:c++latest (or the
  # closest supported flag), so don't also pass an explicit /std: option.
  set(CMAKE_CXX_STANDARD 23)
  add_compile_options(
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /utf-8
    /W4
    /WX
  )

  add_compile_definitions(
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
  )

  if(AOBUS_ENABLE_ASAN OR AOBUS_ENABLE_TSAN)
    message(WARNING "AOBUS_ENABLE_ASAN/AOBUS_ENABLE_TSAN are not wired up for MSVC; ignoring")
  endif()
else()
  # Sanitizers
  if(AOBUS_ENABLE_ASAN)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
      message(WARNING "AOBUS_ENABLE_ASAN is intended for Debug builds; "
        "${CMAKE_BUILD_TYPE} build type is not recommended")
    endif()
    if(NOT CMAKE_CXX_FLAGS MATCHES "fsanitize")
      set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer")
    endif()
    set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
  endif()

  if(AOBUS_ENABLE_TSAN)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
      message(WARNING "AOBUS_ENABLE_TSAN is intended for Debug builds; "
        "${CMAKE_BUILD_TYPE} build type is not recommended")
    endif()
    if(NOT CMAKE_CXX_FLAGS MATCHES "fsanitize")
      set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer")
    endif()
    set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
  endif()

  # Warnings
  set(COMMON_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wunused
    -Wformat=2
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    -Wcast-align
  )

  # Disable -Werror when using sanitizers to avoid noise from third-party headers.
  if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
    add_compile_options(${COMMON_WARNINGS})
  else()
    # Enable -Werror for common clean warnings in non-sanitizer builds.
    add_compile_options(${COMMON_WARNINGS} -Werror)

    # Keep the noisiest warnings disabled globally until third-party headers are
    # consistently isolated behind SYSTEM include paths or local diagnostic guards.
    add_compile_options(
      -Wno-null-dereference
      -Wno-double-promotion
      -Wno-conversion
      -Wno-sign-conversion
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-old-style-cast>
      -Wno-float-conversion
    )
  endif()

  # Fast Linker
  option(USE_FAST_LINKER "Try to use mold or lld for faster linking if available" ON)

  if(USE_FAST_LINKER)
    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=mold -Wl,--version
      OUTPUT_VARIABLE MOLD_VERSION ERROR_QUIET)
    if(MOLD_VERSION)
      set(FAST_LINKER_FLAGS "-fuse-ld=mold")
    else()
      execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=lld -Wl,--version
        OUTPUT_VARIABLE LLD_VERSION ERROR_QUIET)
      if(LLD_VERSION)
        set(FAST_LINKER_FLAGS "-fuse-ld=lld")
      endif()
    endif()

    if(FAST_LINKER_FLAGS)
      set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} ${FAST_LINKER_FLAGS}")
      set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} ${FAST_LINKER_FLAGS}")
      set(CMAKE_MODULE_LINKER_FLAGS
        "${CMAKE_MODULE_LINKER_FLAGS} ${FAST_LINKER_FLAGS}")
      message(STATUS "Using fast linker: ${FAST_LINKER_FLAGS}")
    endif()
  endif()

  # Compiler Cache
  option(USE_CCACHE "Use ccache for faster recompilation if available" ON)

  if(USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
      message(STATUS "Using compiler cache: ${CCACHE_PROGRAM}")
      set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
      set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    else()
      message(STATUS "ccache not found, proceeding without compiler cache.")
    endif()
  endif()
endif()

# spdlog
# Force spdlog call sites to use C++20 std::format instead of the fmt library.
# Dependencies.cmake verifies the located spdlog package matches this ABI.
add_compile_definitions(SPDLOG_USE_STD_FORMAT)

# GSL-lite
# Debug fails fast through assertions; non-Debug disables contract checks.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(gsl_CONFIG_CONTRACT_VIOLATION_ASSERTS=1)
else()
  add_compile_definitions(gsl_CONFIG_CONTRACT_CHECKING_OFF=1)
endif()
