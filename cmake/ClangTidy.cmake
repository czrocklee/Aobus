# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2025 RockStudio Contributors
#
# Clang-tidy discovery, configuration, and per-target application helper.
#
# Usage:
#   aobus_apply_clang_tidy(<target> STRICT)   # lib, tool, app targets
#   aobus_apply_clang_tidy(<target> RELAXED)  # test targets

unset(CLANG_TIDY_EXECUTABLE CACHE)
unset(CLANG_TIDY_EXECUTABLE)

if(AOBUS_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
endif()

if(CLANG_TIDY_EXECUTABLE)
  # Build GCC system include paths for clang-tidy
  set(_AO_CLANG_TIDY_EXTRA_ARGS)
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
      list(APPEND _AO_CLANG_TIDY_EXTRA_ARGS --extra-arg-before=-isystem${CXX_INCLUDE_DIR})
    endif()
  endforeach()

  # Strict checks (lib, tool, app)
  set(_AO_CLANG_TIDY_STRICT_CHECKS
    "-checks=-*,bugprone-*,performance-*,cppcoreguidelines-*,-cppcoreguidelines-avoid-magic-numbers,-cppcoreguidelines-avoid-const-or-ref-data-members,modernize-!-trailing-return,readability-*,-readability-redundant-member-init,portability-*,-clang-diagnostic-*,-bugprone-easily-swappable-parameters,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-readability-convert-member-functions-to-static,-readability-static-definition-in-anonymous-namespace,-clang-diagnostic-note"
  )

  set(_AO_CLANG_TIDY_STRICT_CONFIG
    "{Checks: '-*,readability-identifier-length,readability-magic-numbers,readability-qualified-auto,readability-function-cognitive-complexity', CheckOptions: [{key: 'readability-identifier-length.MinimumVariableNameLength', value: 2}, {key: 'readability-identifier-length.MinimumParameterNameLength', value: 2}, {key: 'readability-identifier-length.IgnoredVariableNames', value: '^[_]$'}, {key: 'readability-identifier-length.IgnoredBindingNames', value: '^[_]$'}, {key: 'readability-magic-numbers.IgnorePowersOf2IntegerValues', value: true}, {key: 'readability-magic-numbers.IgnoredIntegerValues', value: '24;1000;1000U;60;100'}, {key: 'readability-qualified-auto.AllowedTypes', value: 'std::array<.*>::(const_)?iterator;std::string_view::(const_)?iterator;.*::iterator;.*Iterator'}, {key: 'readability-function-cognitive-complexity.Threshold', value: 30}]}"
  )

  # Relaxed checks (test)
  # Tests inherit all STRICT checks but disable rules that heavily conflict
  # with testing frameworks (Catch2/FakeIt) and common test patterns.
  set(_AO_CLANG_TIDY_RELAXED_CHECKS "${_AO_CLANG_TIDY_STRICT_CHECKS}")

  # Allow REQUIRE/CHECK unwrapping without explicit has_value() checks
  string(APPEND _AO_CLANG_TIDY_RELAXED_CHECKS ",-bugprone-unchecked-optional-access")

  # Allow hardcoded test data and inline literals
  string(APPEND _AO_CLANG_TIDY_RELAXED_CHECKS ",-readability-magic-numbers,-cppcoreguidelines-avoid-magic-numbers")

  # Allow complex SECTION() nesting and large test fixtures
  string(APPEND _AO_CLANG_TIDY_RELAXED_CHECKS ",-readability-function-cognitive-complexity")

  # Allow short variable names (e.g., i, j, b) common in isolated test cases
  string(APPEND _AO_CLANG_TIDY_RELAXED_CHECKS ",-readability-identifier-length")

  # Allow Catch2/FakeIt internal C-style casts, variadic macros, and C-arrays
  string(APPEND _AO_CLANG_TIDY_RELAXED_CHECKS ",-cppcoreguidelines-pro-type-reinterpret-cast,-cppcoreguidelines-pro-type-vararg,-cppcoreguidelines-avoid-c-arrays")

  set(_AO_CLANG_TIDY_RELAXED_CONFIG
    "${_AO_CLANG_TIDY_STRICT_CONFIG}"
  )

  # Header filters (portable — uses CMAKE_SOURCE_DIR instead of hardcoded path)
  set(_AO_CLANG_TIDY_STRICT_FILTER "'${CMAKE_SOURCE_DIR}/(lib|app|include)/.*'")
  set(_AO_CLANG_TIDY_RELAXED_FILTER "'${CMAKE_SOURCE_DIR}/(test|include)/.*'")

  message(STATUS "clang-tidy found: ${CLANG_TIDY_EXECUTABLE}")
  message(STATUS "clang-tidy strict checks for lib/app, relaxed for test/")
endif()

# Apply clang-tidy to a target with the given mode (STRICT or RELAXED).
function(aobus_apply_clang_tidy target mode)
  if(NOT CLANG_TIDY_EXECUTABLE)
    return()
  endif()

  if(mode STREQUAL "STRICT")
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${_AO_CLANG_TIDY_STRICT_CHECKS};--config=${_AO_CLANG_TIDY_STRICT_CONFIG};-header-filter=${_AO_CLANG_TIDY_STRICT_FILTER};${_AO_CLANG_TIDY_EXTRA_ARGS}"
    )
  elseif(mode STREQUAL "RELAXED")
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${_AO_CLANG_TIDY_RELAXED_CHECKS};--config=${_AO_CLANG_TIDY_RELAXED_CONFIG};-header-filter=${_AO_CLANG_TIDY_RELAXED_FILTER};${_AO_CLANG_TIDY_EXTRA_ARGS}"
    )
  else()
    message(FATAL_ERROR "aobus_apply_clang_tidy: unknown mode '${mode}' (expected STRICT or RELAXED)")
  endif()
endfunction()
