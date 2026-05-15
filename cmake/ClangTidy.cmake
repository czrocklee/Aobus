# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2025 Aobus Contributors
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
  add_subdirectory(lint)
endif()

option(AOBUS_CLANG_TIDY_FIX "Apply clang-tidy fixes automatically" OFF)

if(CLANG_TIDY_EXECUTABLE)
  # Build GCC system include paths for clang-tidy
  set(_AO_CLANG_TIDY_EXTRA_ARGS)
  
  # Load custom plugin if it exists
  if(TARGET AobusLintPlugin)
    list(APPEND _AO_CLANG_TIDY_EXTRA_ARGS "-load=$<TARGET_FILE:AobusLintPlugin>")
    message(STATUS "clang-tidy: Using custom plugin AobusLintPlugin")
  endif()

  if(AOBUS_CLANG_TIDY_FIX)
    list(APPEND _AO_CLANG_TIDY_EXTRA_ARGS "-fix")
  endif()

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
    "-checks=-*,aobus-*,bugprone-*,performance-*, -performance-unnecessary-value-param,  -performance-move-const-arg, cppcoreguidelines-*,-cppcoreguidelines-avoid-magic-numbers,-cppcoreguidelines-avoid-const-or-ref-data-members,misc-*,-misc-no-recursion,-misc-non-private-member-variables-in-classes, modernize-*,-modernize-use-trailing-return-type,-modernize-use-nodiscard,-modernize-use-auto,readability-*,-readability-redundant-member-init,portability-*, -portability-avoid-pragma-once, -clang-diagnostic-*,-bugprone-easily-swappable-parameters,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-readability-convert-member-functions-to-static,-clang-diagnostic-note"
  )

  set(_AO_CLANG_TIDY_STRICT_CONFIG
    "{Checks: '-*,aobus-*,readability-identifier-length,readability-identifier-naming,readability-magic-numbers,readability-qualified-auto,readability-function-cognitive-complexity', CheckOptions: [{key: 'readability-identifier-length.MinimumVariableNameLength', value: 2}, {key: 'readability-identifier-length.MinimumParameterNameLength', value: 2}, {key: 'readability-identifier-length.IgnoredVariableNames', value: '^[_]([^_].*)?$'}, {key: 'readability-identifier-length.IgnoredBindingNames', value: '^[_]$'}, {key: 'readability-identifier-naming.ClassCase', value: 'CamelCase'}, {key: 'readability-identifier-naming.StructCase', value: 'CamelCase'}, {key: 'readability-identifier-naming.EnumCase', value: 'CamelCase'}, {key: 'readability-identifier-naming.ScopedEnumConstantCase', value: 'CamelCase'}, {key: 'readability-identifier-naming.ConstexprVariableCase', value: 'CamelCase'}, {key: 'readability-identifier-naming.ConstexprVariablePrefix', value: 'k'}, {key: 'readability-identifier-naming.FunctionCase', value: 'camelBack'}, {key: 'readability-identifier-naming.MethodCase', value: 'camelBack'}, {key: 'readability-identifier-naming.MethodIgnoredRegexp', value: '^property_.*|^signal_.*|^vfunc_.*|^on_.*'}, {key: 'readability-identifier-naming.PublicMemberCase', value: 'camelBack'}, {key: 'readability-identifier-naming.TypeAliasCase', value: 'CamelCase'}, {key: 'readability-identifier-naming.TypeAliasIgnoredRegexp', value: '^(difference_type|value_type|pointer|reference|iterator_category)$'}, {key: 'readability-magic-numbers.IgnorePowersOf2IntegerValues', value: true}, {key: 'readability-magic-numbers.IgnoredIntegerValues', value: '24;1000;1000U;60;100'}, {key: 'readability-qualified-auto.AllowedTypes', value: 'std::array<.*>::(const_)?iterator;std::string_view::(const_)?iterator;.*::iterator;.*Iterator'}, {key: 'readability-function-cognitive-complexity.Threshold', value: 30}]}"
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

  # Allow ignored return values in tests (common for command bus execution)
  string(APPEND _AO_CLANG_TIDY_RELAXED_CHECKS ",-bugprone-unused-return-value")

  # Allow Catch2/FakeIt internal C-style casts, variadic macros, and C-arrays
  string(APPEND _AO_CLANG_TIDY_RELAXED_CHECKS ",-cppcoreguidelines-pro-type-reinterpret-cast,-cppcoreguidelines-pro-type-vararg,-cppcoreguidelines-avoid-c-arrays")
  # FakeIt template instantiation triggers this in test/external
  string(APPEND _AO_CLANG_TIDY_RELAXED_CHECKS ",-portability-template-virtual-member-function")

  set(_AO_CLANG_TIDY_RELAXED_CONFIG
    "${_AO_CLANG_TIDY_STRICT_CONFIG}"
  )

  # Header filters (portable — uses CMAKE_SOURCE_DIR instead of hardcoded path)
  set(_AO_CLANG_TIDY_STRICT_FILTER "${CMAKE_SOURCE_DIR}/(lib|app|include)/.*")
  set(_AO_CLANG_TIDY_RELAXED_FILTER "${CMAKE_SOURCE_DIR}/(test|include)/.*")

  message(STATUS "clang-tidy found: ${CLANG_TIDY_EXECUTABLE}")
  message(STATUS "clang-tidy strict checks for lib/app, relaxed for test/")
endif()

# Apply clang-tidy to a target with the given mode (STRICT or RELAXED).
function(aobus_apply_clang_tidy target mode)
  if(NOT CLANG_TIDY_EXECUTABLE)
    return()
  endif()

  if(TARGET AobusLintPlugin)
    add_dependencies(${target} AobusLintPlugin)
  endif()

  if(mode STREQUAL "STRICT")
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${_AO_CLANG_TIDY_EXTRA_ARGS};${_AO_CLANG_TIDY_STRICT_CHECKS};--config=${_AO_CLANG_TIDY_STRICT_CONFIG};-header-filter=${_AO_CLANG_TIDY_STRICT_FILTER}"
    )
  elseif(mode STREQUAL "RELAXED")
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${_AO_CLANG_TIDY_EXTRA_ARGS};${_AO_CLANG_TIDY_RELAXED_CHECKS};--config=${_AO_CLANG_TIDY_RELAXED_CONFIG};-header-filter=${_AO_CLANG_TIDY_RELAXED_FILTER}"
    )
  else()
    message(FATAL_ERROR "aobus_apply_clang_tidy: unknown mode '${mode}' (expected STRICT or RELAXED)")
  endif()
endfunction()
