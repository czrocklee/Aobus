cmake_minimum_required(VERSION 3.30)

project(aobus VERSION 0.1.0)

option(AOBUS_ENABLE_ASAN "Enable address/undefined sanitizers (debug builds only)" OFF)
option(AOBUS_ENABLE_TSAN "Enable thread sanitizer (debug builds only)" OFF)

set(AOBUS_DEFAULT_BUILD_GTK OFF)
if(UNIX AND NOT WIN32)
  set(AOBUS_DEFAULT_BUILD_GTK ON)
endif()

set(AOBUS_DEFAULT_BUILD_TESTS ON)
set(AOBUS_DEFAULT_BUILD_TOOLS ON)
if(WIN32)
  set(AOBUS_DEFAULT_BUILD_TESTS OFF)
  set(AOBUS_DEFAULT_BUILD_TOOLS OFF)
endif()

option(AOBUS_BUILD_CLI "Build the command-line frontend" ON)
option(AOBUS_BUILD_GTK "Build the GTK frontend" ${AOBUS_DEFAULT_BUILD_GTK})
option(AOBUS_BUILD_TESTS "Build test targets" ${AOBUS_DEFAULT_BUILD_TESTS})
option(AOBUS_BUILD_TOOLS "Build developer tools" ${AOBUS_DEFAULT_BUILD_TOOLS})
option(AOBUS_BUILD_LINT_PLUGIN "Build the Aobus clang-tidy plugin" ${AOBUS_BUILD_TOOLS})
option(AOBUS_BUILD_TUI "Build the terminal frontend" ON)

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Gperf.cmake)

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
