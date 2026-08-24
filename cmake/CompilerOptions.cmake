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
  # libc++ constrains std::expected's equality operators only in C++26 mode, and
  # those constraints are self-referential: satisfying
  #
  #   { *__x == __v } -> __core_convertible_to<bool>
  #
  # re-enters the same hidden-friend candidate whenever the right operand's
  # associated entities include std::expected. std::reverse_iterator<expected*>
  # does -- vector's reallocation guard compares exactly those -- and so does
  # asio's promise_executor over a Result-returning coroutine. Satisfaction that
  # depends on itself is an error, so the translation unit fails outright.
  # Reproduced on libc++ 21 and 22; libstdc++ and the MSVC STL do not implement
  # P2944 yet, which is why only macOS breaks.
  #
  # shell.nix builds a copy of that one header with the C++26 clauses disabled
  # and exports its directory. Prepending it on the command line is what makes
  # it win: the real libc++ include directory is built into the compiler driver
  # rather than passed as an argument, and a command-line -isystem is searched
  # before the driver's own directories. CPLUS_INCLUDE_PATH and
  # NIX_CFLAGS_COMPILE were both measured to lose that race.
  #
  # Retirement condition: doc/development/macos-portability.md.
  if(APPLE)
    if(NOT DEFINED ENV{AOBUS_LIBCXX_EXPECTED_SHIM})
      message(FATAL_ERROR
        "AOBUS_LIBCXX_EXPECTED_SHIM is unset. Configure through the ./ao portal, "
        "which enters shell.nix and exports it; without it libc++ cannot compile "
        "std::expected comparisons in C++26 mode.")
    endif()
    add_compile_options(-isystem $ENV{AOBUS_LIBCXX_EXPECTED_SHIM})
  endif()

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

  # fakeit's Method() macro expands to __COUNTER__, which Clang 22 reports as
  # a C2y extension under -Wpedantic. Sanitizer builds need the suppression too;
  # the construct is third-party, not project code.
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-c2y-extensions>)
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
