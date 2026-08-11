cmake_minimum_required(VERSION 3.30)

project(aobus VERSION 0.1.0)

if(NOT WIN32 AND NOT LINUX)
  message(FATAL_ERROR
    "Aobus supports Linux and Windows hosts only; no audio backend or "
    "development profile exists for this platform.")
endif()

option(AOBUS_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer where available (debug builds only)" OFF)
option(AOBUS_ENABLE_TSAN "Enable thread sanitizer (debug builds only)" OFF)

set(AOBUS_DEFAULT_BUILD_GTK OFF)
if(UNIX AND NOT WIN32)
  set(AOBUS_DEFAULT_BUILD_GTK ON)
endif()

set(AOBUS_DEFAULT_BUILD_TOOLS ON)
if(WIN32)
  set(AOBUS_DEFAULT_BUILD_TOOLS OFF)
endif()

option(AOBUS_BUILD_CLI "Build the command-line frontend" ON)
option(AOBUS_BUILD_GTK "Build the GTK frontend" ${AOBUS_DEFAULT_BUILD_GTK})
option(AOBUS_BUILD_TESTS "Build test targets" ON)
option(AOBUS_BUILD_TOOLS "Build developer tools" ${AOBUS_DEFAULT_BUILD_TOOLS})
option(AOBUS_BUILD_LINT_PLUGIN "Build the Aobus clang-tidy plugin" ${AOBUS_BUILD_TOOLS})
option(AOBUS_BUILD_TUI "Build the terminal frontend" ON)
option(AOBUS_BUILD_WINUI "Build the Windows WinUI 3 frontend" OFF)

include(cmake/CompilerOptions.cmake)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR "Release" IN_LIST CMAKE_CONFIGURATION_TYPES)
  if(AOBUS_ENABLE_ASAN OR AOBUS_ENABLE_TSAN)
    message(FATAL_ERROR
      "Release configurations always use IPO and cannot be combined with sanitizers")
  endif()

  include(CheckIPOSupported)
  check_ipo_supported(
    RESULT AOBUS_IPO_SUPPORTED
    OUTPUT AOBUS_IPO_ERROR
    LANGUAGES CXX)
  if(NOT AOBUS_IPO_SUPPORTED)
    message(FATAL_ERROR "IPO is not supported by this toolchain: ${AOBUS_IPO_ERROR}")
  endif()
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

include(cmake/Dependencies.cmake)
include(cmake/Gperf.cmake)

set(AO_GUARDRAIL_REGEX_DIR "${CMAKE_CURRENT_BINARY_DIR}/guardrail-regex")
file(MAKE_DIRECTORY "${AO_GUARDRAIL_REGEX_DIR}")
function(ao_write_guardrail_regex out_var name pattern)
  set(_path "${AO_GUARDRAIL_REGEX_DIR}/${name}.regex")
  file(WRITE "${_path}" "${pattern}")
  set(${out_var} "${_path}" PARENT_SCOPE)
endfunction()

set(AO_FORBIDDEN_C_ASSERT_REGEX "(^|[^A-Za-z0-9_])assert[ \t\r\n]*\\(")
foreach(AO_FORBIDDEN_C_ASSERT_SPELLING IN ITEMS
    "assert(value)"
    " assert (value)"
    "\nassert\n(value)")
  if(NOT "${AO_FORBIDDEN_C_ASSERT_SPELLING}" MATCHES "${AO_FORBIDDEN_C_ASSERT_REGEX}")
    message(FATAL_ERROR "C assert guardrail does not reject '${AO_FORBIDDEN_C_ASSERT_SPELLING}'")
  endif()
endforeach()
foreach(AO_ALLOWED_ASSERT_SPELLING IN ITEMS
    "static_assert(value)"
    "AO_INVARIANT(value)")
  if("${AO_ALLOWED_ASSERT_SPELLING}" MATCHES "${AO_FORBIDDEN_C_ASSERT_REGEX}")
    message(FATAL_ERROR "C assert guardrail rejects '${AO_ALLOWED_ASSERT_SPELLING}'")
  endif()
endforeach()
ao_write_guardrail_regex(AO_FORBIDDEN_C_ASSERT_REGEX_FILE "c-assert" "${AO_FORBIDDEN_C_ASSERT_REGEX}")

add_custom_target(ao_no_c_assert_check ALL
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${CMAKE_SOURCE_DIR}/include;${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_C_ASSERT_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking production sources do not use configuration-dependent C assert"
  VERBATIM
)

set(AO_FORBIDDEN_RAW_GSL_CONTRACT_REGEX
    "(^|[^A-Za-z0-9_])gsl_(Expects|Ensures|Assert)[ \t\r\n]*\\(")
foreach(AO_FORBIDDEN_RAW_GSL_CONTRACT_SPELLING IN ITEMS
    "gsl_Expects(value)"
    " gsl_Ensures (value)"
    "\ngsl_Assert\n(value)")
  if(NOT "${AO_FORBIDDEN_RAW_GSL_CONTRACT_SPELLING}" MATCHES "${AO_FORBIDDEN_RAW_GSL_CONTRACT_REGEX}")
    message(FATAL_ERROR
      "Raw gsl-lite contract guardrail does not reject '${AO_FORBIDDEN_RAW_GSL_CONTRACT_SPELLING}'")
  endif()
endforeach()
foreach(AO_ALLOWED_AO_CONTRACT_SPELLING IN ITEMS
    "AO_EXPECTS(value)"
    "AO_ENSURES(value)"
    "AO_INVARIANT(value)")
  if("${AO_ALLOWED_AO_CONTRACT_SPELLING}" MATCHES "${AO_FORBIDDEN_RAW_GSL_CONTRACT_REGEX}")
    message(FATAL_ERROR
      "Raw gsl-lite contract guardrail rejects '${AO_ALLOWED_AO_CONTRACT_SPELLING}'")
  endif()
endforeach()
ao_write_guardrail_regex(
  AO_FORBIDDEN_RAW_GSL_CONTRACT_REGEX_FILE
  "raw-gsl-contract"
  "${AO_FORBIDDEN_RAW_GSL_CONTRACT_REGEX}")

add_custom_target(ao_no_raw_gsl_contract_check ALL
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${CMAKE_SOURCE_DIR}/include;${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_RAW_GSL_CONTRACT_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking production sources do not use raw gsl-lite contracts"
  VERBATIM
)

set(AO_FORBIDDEN_UNCONDITIONAL_TERMINAL_REGEX
    "(AO_(EXPECTS|ENSURES|INVARIANT)[ \t\r\n]*\\([ \t\r\n]*false([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])std::unreachable[ \t\r\n]*\\()")
foreach(AO_FORBIDDEN_UNCONDITIONAL_TERMINAL_SPELLING IN ITEMS
    "AO_EXPECTS(false)"
    "AO_ENSURES ( false, \"message\")"
    "AO_INVARIANT\n(\nfalse)"
    "std::unreachable()")
  if(NOT "${AO_FORBIDDEN_UNCONDITIONAL_TERMINAL_SPELLING}" MATCHES
      "${AO_FORBIDDEN_UNCONDITIONAL_TERMINAL_REGEX}")
    message(FATAL_ERROR
      "Unconditional-terminal guardrail does not reject '${AO_FORBIDDEN_UNCONDITIONAL_TERMINAL_SPELLING}'")
  endif()
endforeach()
foreach(AO_ALLOWED_UNCONDITIONAL_TERMINAL_SPELLING IN ITEMS
    "AO_EXPECTS(falseValue)"
    "AO_ENSURES(value)"
    "AO_INVARIANT(condition)"
    "AO_FATAL(\"message\")")
  if("${AO_ALLOWED_UNCONDITIONAL_TERMINAL_SPELLING}" MATCHES
     "${AO_FORBIDDEN_UNCONDITIONAL_TERMINAL_REGEX}")
    message(FATAL_ERROR
      "Unconditional-terminal guardrail rejects '${AO_ALLOWED_UNCONDITIONAL_TERMINAL_SPELLING}'")
  endif()
endforeach()
ao_write_guardrail_regex(
  AO_FORBIDDEN_UNCONDITIONAL_TERMINAL_REGEX_FILE
  "unconditional-terminal"
  "${AO_FORBIDDEN_UNCONDITIONAL_TERMINAL_REGEX}")

add_custom_target(ao_no_unconditional_terminal_check ALL
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${CMAKE_SOURCE_DIR}/include;${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_UNCONDITIONAL_TERMINAL_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking unconditional terminal branches use AO_FATAL"
  VERBATIM
)

set(AO_FORBIDDEN_REMOVED_EXCEPTION_SURFACE_REGEX
    "(ao/Exception\\.h|(^|[^A-Za-z0-9_])(ExceptionFormat|throwException)([^A-Za-z0-9_]|$))")
foreach(AO_FORBIDDEN_REMOVED_EXCEPTION_SPELLING IN ITEMS
    "#include <ao/Exception.h>"
    "ExceptionFormat<int>"
    "throwException(value)")
  if(NOT "${AO_FORBIDDEN_REMOVED_EXCEPTION_SPELLING}" MATCHES "${AO_FORBIDDEN_REMOVED_EXCEPTION_SURFACE_REGEX}")
    message(FATAL_ERROR
      "Removed exception-surface guardrail does not reject '${AO_FORBIDDEN_REMOVED_EXCEPTION_SPELLING}'")
  endif()
endforeach()
foreach(AO_ALLOWED_EXCEPTION_SPELLING IN ITEMS
    "std::exception"
    "std::current_exception()"
    "throwOperationCancelled()")
  if("${AO_ALLOWED_EXCEPTION_SPELLING}" MATCHES "${AO_FORBIDDEN_REMOVED_EXCEPTION_SURFACE_REGEX}")
    message(FATAL_ERROR
      "Removed exception-surface guardrail rejects '${AO_ALLOWED_EXCEPTION_SPELLING}'")
  endif()
endforeach()
ao_write_guardrail_regex(
  AO_FORBIDDEN_REMOVED_EXCEPTION_SURFACE_REGEX_FILE
  "removed-exception-surface"
  "${AO_FORBIDDEN_REMOVED_EXCEPTION_SURFACE_REGEX}")

add_custom_target(ao_no_removed_exception_surface_check ALL
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${CMAKE_SOURCE_DIR}/include;${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_REMOVED_EXCEPTION_SURFACE_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking removed general exception APIs do not return"
  VERBATIM
)

# Deliberately keep this lexical guard to qualified/global spellings: Aobus has
# transaction methods named abort(). The AST checker below the build guard owns
# imported and otherwise unqualified standard-function resolution.
set(AO_FORBIDDEN_RAW_FATAL_PRIMITIVE_REGEX
    "(^|[^A-Za-z0-9_])(std::(terminate|abort|quick_exit|_Exit)|::(terminate|abort|quick_exit|_Exit)|_Exit)[ \t\r\n]*\\(")
foreach(AO_FORBIDDEN_RAW_FATAL_SPELLING IN ITEMS
    "std::terminate()"
    "std::abort ()"
    "std::quick_exit(1)"
    "std::_Exit(1)"
    "::abort()"
    "_Exit(1)")
  if(NOT "${AO_FORBIDDEN_RAW_FATAL_SPELLING}" MATCHES "${AO_FORBIDDEN_RAW_FATAL_PRIMITIVE_REGEX}")
    message(FATAL_ERROR "Raw fatal guardrail does not reject '${AO_FORBIDDEN_RAW_FATAL_SPELLING}'")
  endif()
endforeach()
foreach(AO_ALLOWED_FATAL_SPELLING IN ITEMS
    "AO_FATAL(\"context\")"
    "AO_INVARIANT(value)"
    "std::exit(0)"
    "worker.abort()"
    "worker->terminate()"
    "application::abort()")
  if("${AO_ALLOWED_FATAL_SPELLING}" MATCHES "${AO_FORBIDDEN_RAW_FATAL_PRIMITIVE_REGEX}")
    message(FATAL_ERROR "Raw fatal guardrail rejects '${AO_ALLOWED_FATAL_SPELLING}'")
  endif()
endforeach()
ao_write_guardrail_regex(
  AO_FORBIDDEN_RAW_FATAL_PRIMITIVE_REGEX_FILE
  "raw-fatal-primitive"
  "${AO_FORBIDDEN_RAW_FATAL_PRIMITIVE_REGEX}")

add_custom_target(ao_no_raw_fatal_primitive_check ALL
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${CMAKE_SOURCE_DIR}/include;${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_RAW_FATAL_PRIMITIVE_REGEX_FILE}"
          "-DEXCLUDE_REGEX=.*/lib/utility/Fatal\\.cpp$"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking raw fatal primitives stay inside the AO fatal backend"
  VERBATIM
)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/ao")
configure_file(cmake/AppVersion.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ao/AppVersion.h" @ONLY)

if(AOBUS_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(lib)
add_subdirectory(app)
if(AOBUS_BUILD_TOOLS OR AOBUS_BUILD_LINT_PLUGIN)
  add_subdirectory(tool)
endif()
if(AOBUS_BUILD_TESTS)
  add_subdirectory(test)
endif()

get_property(AOBUS_GENERATED_HEADERS GLOBAL PROPERTY AOBUS_GENERATED_HEADERS)
if(AOBUS_GENERATED_HEADERS)
  add_custom_target(aobus_generated_headers DEPENDS ${AOBUS_GENERATED_HEADERS})
endif()
