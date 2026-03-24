cmake_minimum_required(VERSION 3.30)

project(rockstudio)
set (CMAKE_CXX_STANDARD 23)

# Enable testing
enable_testing()

# Strict compiler warnings (disable -Werror when using sanitizers, also disable for third-party headers)
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
  add_compile_options(-Wall -Wextra -Wpedantic)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-error)
endif()

# GSL-lite configuration: Debug throws exceptions, Release terminates
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(gsl_CONFIG_CONTRACT_VIOLATION_THROWS=1)
else()
  add_compile_definitions(gsl_CONFIG_CONTRACT_VIOLATION_TERMINATES=1)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(lmdb REQUIRED)
pkg_search_module(MIMALLOC REQUIRED mimalloc)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)

find_program(GLIB_COMPILE_RESOURCES_EXECUTABLE glib-compile-resources REQUIRED)

add_library(GTK4 INTERFACE)
target_include_directories(GTK4 INTERFACE ${GTK4_INCLUDE_DIRS})
target_link_libraries(GTK4 INTERFACE ${GTK4_LIBRARIES})
target_compile_options(GTK4 INTERFACE ${GTK4_CFLAGS_OTHER})

add_library(GTKMM INTERFACE)
target_include_directories(GTKMM INTERFACE ${GTKMM_INCLUDE_DIRS})
target_link_libraries(GTKMM INTERFACE ${GTKMM_LIBRARIES})
target_compile_options(GTKMM INTERFACE ${GTKMM_CFLAGS_OTHER})

find_package(Boost REQUIRED COMPONENTS headers program_options locale timer filesystem)

# Catch2 via pkg-config
pkg_check_modules(CATCH2 REQUIRED catch2)
add_library(Catch2 INTERFACE)
target_include_directories(Catch2 INTERFACE ${CATCH2_CFLAGS})
target_link_libraries(Catch2 INTERFACE ${CATCH2_LDFLAGS})

find_package(Qt6 COMPONENTS Core Widgets REQUIRED)

set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# clang-tidy setup (must be before rs target to enable CMAKE_CXX_CLANG_TIDY)
find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
if(CLANG_TIDY_EXECUTABLE)
  # Set up GCC include paths for clang-tidy (required for GCC)
  set(CLANG_TIDY_EXTRA_ARGS)
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
      list(APPEND CLANG_TIDY_EXTRA_ARGS --extra-arg-before=-isystem${CXX_INCLUDE_DIR})
    endif()
  endforeach()

  # clang-tidy configuration: only analyze project source files
  # Use --header-filter to limit to files under the project directory
  set(CMAKE_CXX_CLANG_TIDY
    "${CLANG_TIDY_EXECUTABLE}"
    "-header-filter='/home/rocklee/dev/RockStudio/.*'"
    ${CLANG_TIDY_EXTRA_ARGS}
    CACHE STRING "clang-tidy command"
  )

  message(STATUS "clang-tidy found: ${CLANG_TIDY_EXECUTABLE}")
  message(STATUS "clang-tidy will run automatically during build (incremental + parallel)")
else()
  message(STATUS "clang-tidy not found - install it for linting")
endif()

add_library(rs OBJECT
    src/lmdb/Environment.cpp
    src/lmdb/Transaction.cpp
    src/lmdb/Database.cpp
    src/core/MusicLibrary.cpp
    src/core/ResourceStore.cpp
    src/core/DictionaryStore.cpp
    src/core/TrackStore.cpp
    src/core/TrackRecord.cpp
    src/core/TrackLayout.cpp
    src/core/ListLayout.cpp
    src/core/ListStore.cpp
    src/expr/ExecutionPlan.cpp
    src/expr/PlanEvaluator.cpp
    src/expr/Expression.cpp
    src/expr/Parser.cpp
    src/expr/Serializer.cpp
    src/tag/File.cpp
    src/tag/flac/File.cpp
    src/tag/flac/MetadataBlock.cpp
    src/tag/mp4/File.cpp
    src/tag/mp4/Atom.cpp
    src/tag/mpeg/File.cpp
    src/tag/mpeg/Frame.cpp
    src/tag/mpeg/id3v2/Reader.cpp
)
target_include_directories(rs PUBLIC include)
target_link_libraries(rs lmdb Boost::locale)

add_executable(rsc
    tool/main.cpp
    tool/InitCommand.cpp
    tool/ListCommand.cpp
    tool/TagCommand.cpp
    tool/TrackCommand.cpp
)
target_link_libraries(rsc PRIVATE rs Qt6::Core Boost::program_options)

add_executable(rs_test
    test/main.cpp
    test/query/ParserTest.cpp
    test/query/ExecutionPlanTest.cpp
    test/query/PlanEvaluatorTest.cpp
    test/core/TrackLayoutTest.cpp
    test/core/TrackRecordTest.cpp
    test/core/ListLayoutTest.cpp
    test/core/DictionaryTest.cpp
    test/core/ResourceStoreTest.cpp
    test/core/TrackStoreTest.cpp
    test/core/ListStoreTest.cpp
    test/lmdb/EnvironmentTest.cpp
    test/lmdb/TransactionTest.cpp
    test/lmdb/DatabaseTest.cpp
)
target_include_directories(rs_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(rs_test PRIVATE rs Catch2)
target_compile_options(rs_test PRIVATE -Wno-array-bounds)

# Register rs_test with CTest
add_test(NAME rs_test COMMAND rs_test)

# UI disabled - requires FlatBuffers migration
# add_executable(RockStudio
#     app/ImportProgressDialog.cpp
#     ...
# )
#
# add_executable(RockStudioGTKmm4
#     app/gtkmm4/main.cpp
#     ...
# )
