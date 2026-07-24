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
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
  )

  if(AOBUS_ENABLE_TSAN)
    message(FATAL_ERROR "ThreadSanitizer is not supported by the MSVC Windows toolchain")
  endif()

  if(AOBUS_ENABLE_ASAN)
    # MSVC AddressSanitizer is incompatible with the /RTC options that CMake
    # adds to Debug flags by default and with incremental linking.
    foreach(flags_var CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
      string(REGEX REPLACE "(^| )[/-]RTC[1su]*($| )" " " ${flags_var} "${${flags_var}}")
    endforeach()
    foreach(flags_var CMAKE_EXE_LINKER_FLAGS_DEBUG CMAKE_SHARED_LINKER_FLAGS_DEBUG CMAKE_MODULE_LINKER_FLAGS_DEBUG)
      string(REGEX REPLACE "(^| )[/-]INCREMENTAL(:YES)?($| )" " " ${flags_var} "${${flags_var}}")
    endforeach()
    # The normal vcpkg triplet is not ASan-instrumented. Keep MSVC STL's
    # detect_mismatch records compatible across that binary boundary while
    # retaining AddressSanitizer instrumentation in Aobus translation units.
    add_compile_definitions(_DISABLE_STL_ANNOTATION)
    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO)
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
# Contracts are part of the runtime safety model in every configuration. Debug
# keeps assertion diagnostics; optimized builds terminate on violations rather
# than compiling the checks out and continuing into undefined behavior.
add_compile_definitions(gsl_CONFIG_CONTRACT_CHECKING_ON)
add_compile_definitions(
  $<$<CONFIG:Debug>:gsl_CONFIG_CONTRACT_VIOLATION_ASSERTS>
  $<$<NOT:$<CONFIG:Debug>>:gsl_CONFIG_CONTRACT_VIOLATION_TERMINATES>
)
