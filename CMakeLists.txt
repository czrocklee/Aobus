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
include(cmake/Dependencies.cmake)
include(cmake/Gperf.cmake)

set(AO_GUARDRAIL_REGEX_DIR "${CMAKE_CURRENT_BINARY_DIR}/guardrail-regex")
file(MAKE_DIRECTORY "${AO_GUARDRAIL_REGEX_DIR}")
function(ao_write_guardrail_regex out_var name pattern)
  set(_path "${AO_GUARDRAIL_REGEX_DIR}/${name}.regex")
  file(WRITE "${_path}" "${pattern}")
  set(${out_var} "${_path}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/ao")
configure_file(cmake/AppVersion.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ao/AppVersion.h" @ONLY)

# Native audio backend availability, consumed as <ao/audio/BackendConfig.h>.
if(WIN32)
  set(AOBUS_HAS_WASAPI 1)
elseif(LINUX)
  set(AOBUS_HAS_PIPEWIRE 1)
  set(AOBUS_HAS_ALSA 1)
endif()
configure_file(cmake/AudioBackendConfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ao/audio/BackendConfig.h" @ONLY)

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
