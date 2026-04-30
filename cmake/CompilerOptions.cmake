# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2025 RockStudio Contributors
#
# Compiler and linker configuration for the RockStudio project.

# ── C++ Standard ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)

# ── Sanitizers ──────────────────────────────────────────────────────────────
if(ROCKSTUDIO_ENABLE_ASAN)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(WARNING "ROCKSTUDIO_ENABLE_ASAN is intended for Debug builds; "
      "${CMAKE_BUILD_TYPE} build type is not recommended")
  endif()
  if(NOT CMAKE_CXX_FLAGS MATCHES "fsanitize")
    set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer")
  endif()
  set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
endif()

# ── Warnings ────────────────────────────────────────────────────────────────
# Disable -Werror when using sanitizers to avoid noise from third-party headers.
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
  add_compile_options(-Wall -Wextra -Wpedantic)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-error)
endif()

# ── GSL-lite ────────────────────────────────────────────────────────────────
# Debug throws exceptions, non-Debug disables contract checks.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(gsl_CONFIG_CONTRACT_VIOLATION_ASSERTS=1)
else()
  add_compile_definitions(gsl_CONFIG_CONTRACT_CHECKING_OFF=1)
endif()

# ── Fast Linker ─────────────────────────────────────────────────────────────
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
