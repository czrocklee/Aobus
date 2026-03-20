cmake_minimum_required(VERSION 3.30)

project(rockstudio)
set (CMAKE_CXX_STANDARD 23)

# Precompiled Headers support
set(CMAKE_CXX_PRECOMPILE_HEADERS ON)

# Strict compiler warnings (disable -Werror when using sanitizers)
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
  add_compile_options(-Wall -Wextra -Wpedantic)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
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
target_precompile_headers(rs PRIVATE include/rs/rs_pch.h)

add_executable(rsc
    tool/main.cpp
    tool/InitCommand.cpp
    tool/ListCommand.cpp
    tool/TagCommand.cpp
    tool/TrackCommand.cpp
)
target_link_libraries(rsc PRIVATE rs Qt6::Core Boost::program_options)

add_executable(rs_test
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

# Lint target for clang-tidy (cppgen enforcement)
# Checks are defined in .clang-tidy
find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
if(CLANG_TIDY_EXECUTABLE)
  # Get source files for rs library
  get_target_property(RS_SOURCES rs SOURCES)

  add_custom_target(clang-tidy
    COMMAND ${CLANG_TIDY_EXECUTABLE}
      -header-filter='.*'
      -p=${CMAKE_SOURCE_DIR}/compile_commands.json
      ${RS_SOURCES}
    COMMENT "Running clang-tidy on rs library..."
    VERBATIM
  )

  message(STATUS "clang-tidy found: ${CLANG_TIDY_EXECUTABLE}")
  message(STATUS "Run 'cmake --build . --target clang-tidy' to lint")
else()
  message(STATUS "clang-tidy not found - install it for linting")
endif()
