cmake_minimum_required(VERSION 3.30)

project(rockstudio VERSION 0.1.0)

option(ROCKSTUDIO_ENABLE_CLANG_TIDY "Enable clang-tidy during builds" OFF)
option(ROCKSTUDIO_ENABLE_ASAN "Enable address/undefined sanitizers (debug builds only)" OFF)

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)
include(cmake/ClangTidy.cmake)
include(cmake/Gperf.cmake)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/rs")
configure_file(cmake/AppVersion.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/rs/AppVersion.h" @ONLY)

enable_testing()

add_subdirectory(lib)
add_subdirectory(tool)
add_subdirectory(app)
add_subdirectory(test)