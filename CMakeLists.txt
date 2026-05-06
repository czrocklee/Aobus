cmake_minimum_required(VERSION 3.30)

project(aobus VERSION 0.1.0)

option(AOBUS_ENABLE_CLANG_TIDY "Enable clang-tidy during builds" OFF)
option(AOBUS_ENABLE_ASAN "Enable address/undefined sanitizers (debug builds only)" OFF)
option(AOBUS_ENABLE_TSAN "Enable thread sanitizer (debug builds only)" OFF)

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)
include(cmake/ClangTidy.cmake)
include(cmake/Gperf.cmake)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/ao")
configure_file(cmake/AppVersion.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ao/AppVersion.h" @ONLY)

enable_testing()

add_subdirectory(lib)
add_subdirectory(app)
add_subdirectory(test)