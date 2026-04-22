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

# GSL-lite configuration: Debug throws exceptions, non-Debug disables contract checks
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(gsl_CONFIG_CONTRACT_VIOLATION_ASSERTS=1)
else()
  add_compile_definitions(gsl_CONFIG_CONTRACT_CHECKING_OFF=1)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(lmdb REQUIRED)
pkg_search_module(MIMALLOC REQUIRED mimalloc)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)

find_program(GLIB_COMPILE_RESOURCES_EXECUTABLE glib-compile-resources REQUIRED)
find_program(GPERF_EXECUTABLE gperf REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Gperf.cmake)

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

# FFmpeg via pkg-config (optional - playback feature)
# Try both naming conventions: ffmpeg uses "libavformat" style in nixpkgs
pkg_check_modules(AVFORMAT libavformat)
pkg_check_modules(AVCODEC libavcodec)
pkg_check_modules(AVUTIL libavutil)
pkg_check_modules(SWRESAMPLE libswresample)
if(NOT AVFORMAT_FOUND)
  # Fallback to ffmpeg naming
  pkg_check_modules(AVFORMAT avformat)
  pkg_check_modules(AVCODEC avcodec)
  pkg_check_modules(AVUTIL avutil)
  pkg_check_modules(SWRESAMPLE swresample)
endif()
if(AVFORMAT_FOUND AND AVCODEC_FOUND AND AVUTIL_FOUND AND SWRESAMPLE_FOUND)
  set(FFmpeg_FOUND TRUE)
  add_library(FFmpeg INTERFACE)
  target_include_directories(FFmpeg INTERFACE ${AVFORMAT_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${SWRESAMPLE_INCLUDE_DIRS})
  target_link_libraries(FFmpeg INTERFACE ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${SWRESAMPLE_LIBRARIES})
  message(STATUS "FFmpeg found: playback support enabled")
else()
  set(FFmpeg_FOUND FALSE)
  message(STATUS "FFmpeg not found: playback support disabled")
endif()

# PipeWire via pkg-config (optional - audio backend)
pkg_check_modules(PIPEWIRE libpipewire-0.3)
if(PIPEWIRE_FOUND)
  add_library(PipeWire INTERFACE)
  target_include_directories(PipeWire SYSTEM INTERFACE ${PIPEWIRE_INCLUDE_DIRS})
  target_link_libraries(PipeWire INTERFACE ${PIPEWIRE_LIBRARIES})
  message(STATUS "PipeWire found: audio backend enabled")
else()
  message(STATUS "PipeWire not found: audio backend disabled")
endif()

# ALSA via pkg-config (optional - audio backend)
pkg_check_modules(ALSA alsa)
if(ALSA_FOUND)
  add_library(ALSA INTERFACE)
  target_include_directories(ALSA INTERFACE ${ALSA_INCLUDE_DIRS})
  target_link_libraries(ALSA INTERFACE ${ALSA_LIBRARIES})
  message(STATUS "ALSA found: ALSA exclusive backend enabled")
else()
  message(STATUS "ALSA not found: ALSA exclusive backend disabled")
endif()

# clang-tidy setup (manual application to targets)
#find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
unset(CLANG_TIDY_EXECUTABLE)

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

  # clang-tidy checks for strict targets (src, tool)
  set(CLANG_TIDY_STRICT_CHECKS
    "-checks=-*,bugprone-*,performance-*,cppcoreguidelines-*,-cppcoreguidelines-avoid-const-or-ref-data-members,modernize-!-trailing-return,readability-*,portability-*,-clang-diagnostic-*,-bugprone-easily-swappable-parameters,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-readability-convert-member-functions-to-static,-readability-static-definition-in-anonymous-namespace"
  )

  # clang-tidy checks for relaxed test targets (minimal checks)
  set(CLANG_TIDY_RELAXED_CHECKS
    "-checks=-*,performance-avoid-endl"
  )

  # Strict header filter (exclude test/)
  set(CLANG_TIDY_STRICT_FILTER "'/home/rocklee/dev/RockStudio/(src|include|app|tool)/.*'")
  set(CLANG_TIDY_RELAXED_FILTER "'/home/rocklee/dev/RockStudio/test/.*'")

  message(STATUS "clang-tidy found: ${CLANG_TIDY_EXECUTABLE}")
  message(STATUS "clang-tidy strict checks for src/tool, relaxed for test/")
else()
  message(STATUS "clang-tidy not found - install it for linting")
endif()

add_gperf_header(
  FLAC_VORBIS_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tag/flac/VorbisCommentDispatch.gperf"
  "tag/flac/VorbisCommentDispatch.h"
)
add_gperf_header(
  MP4_ATOM_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tag/mp4/AtomDispatch.gperf"
  "tag/mp4/AtomDispatch.h"
)
add_gperf_header(
  MPEG_FRAME_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tag/mpeg/id3v2/FrameDispatch.gperf"
  "tag/mpeg/id3v2/FrameDispatch.h"
)
add_gperf_header(
  EXPR_property_dispatch_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/src/expr/PropertyDispatch.gperf"
  "expr/PropertyDispatch.h"
)
add_gperf_header(
  EXPR_metadata_dispatch_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/src/expr/MetadataDispatch.gperf"
  "expr/MetadataDispatch.h"
)
add_gperf_header(
  EXPR_UNIT_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/src/expr/UnitDispatch.gperf"
  "expr/UnitDispatch.h"
)
add_custom_target(
  tag_dispatch_headers
  DEPENDS
    "${FLAC_VORBIS_DISPATCH_HEADER}"
    "${MP4_ATOM_DISPATCH_HEADER}"
    "${MPEG_FRAME_DISPATCH_HEADER}"
    "${EXPR_property_dispatch_HEADER}"
    "${EXPR_metadata_dispatch_HEADER}"
    "${EXPR_UNIT_DISPATCH_HEADER}"
)

add_library(rs OBJECT
    src/lmdb/Environment.cpp
    src/lmdb/Transaction.cpp
    src/lmdb/Database.cpp
    src/core/LibraryMetaStore.cpp
    src/core/MusicLibrary.cpp
    src/core/ResourceStore.cpp
    src/core/DictionaryStore.cpp
    src/core/TrackStore.cpp
    src/core/TrackBuilder.cpp
    src/core/TrackView.cpp
    src/core/ListView.cpp
    src/core/ListStore.cpp
    src/core/ListBuilder.cpp
    src/expr/ExecutionPlan.cpp
    src/expr/PlanEvaluator.cpp
    src/expr/Expression.cpp
    src/expr/Parser.cpp
    src/expr/Serializer.cpp
    src/tag/File.cpp
    src/tag/Open.cpp
    src/tag/flac/File.cpp
    src/tag/flac/MetadataBlock.cpp
    src/tag/mp4/File.cpp
    src/tag/mp4/Atom.cpp
    src/tag/mpeg/File.cpp
    src/tag/mpeg/Frame.cpp
    src/tag/mpeg/id3v2/Reader.cpp
    "${FLAC_VORBIS_DISPATCH_HEADER}"
    "${MP4_ATOM_DISPATCH_HEADER}"
    "${EXPR_property_dispatch_HEADER}"
    "${EXPR_metadata_dispatch_HEADER}"
    "${EXPR_UNIT_DISPATCH_HEADER}"
)
target_include_directories(rs PUBLIC include)
target_include_directories(rs PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(rs lmdb Boost::locale)
add_dependencies(rs tag_dispatch_headers)
if(CLANG_TIDY_EXECUTABLE)
  set_target_properties(rs PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_STRICT_CHECKS};-header-filter=${CLANG_TIDY_STRICT_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
  )
endif()

add_executable(rsc
    tool/main.cpp
    tool/InitCommand.cpp
    tool/LibCommand.cpp
    tool/ListCommand.cpp
    tool/TagCommand.cpp
    tool/TrackCommand.cpp
)
target_link_libraries(rsc PRIVATE rs Boost::program_options)
if(CLANG_TIDY_EXECUTABLE)
  set_target_properties(rsc PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_STRICT_CHECKS};-header-filter=${CLANG_TIDY_STRICT_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
  )
endif()

add_executable(rs_test
    test/main.cpp
    test/unit/query/ParserTest.cpp
    test/unit/query/ExecutionPlanTest.cpp
    test/unit/query/PlanEvaluatorTest.cpp
    test/unit/core/TrackLayoutTest.cpp
    test/unit/core/TrackViewTest.cpp
    test/unit/core/TrackRecordTest.cpp
    test/unit/core/TrackBuilderTest.cpp
    test/unit/core/ListLayoutTest.cpp
    test/unit/core/ListViewTest.cpp
    test/unit/core/DictionaryTest.cpp
    test/unit/core/ResourceStoreTest.cpp
    test/unit/core/TrackStoreTest.cpp
    test/unit/core/ListStoreTest.cpp
    test/unit/core/ListBuilderTest.cpp
    test/unit/core/MusicLibraryTest.cpp
    test/unit/lmdb/EnvironmentTest.cpp
    test/unit/lmdb/TransactionTest.cpp
    test/unit/lmdb/DatabaseTest.cpp
    test/unit/app/TrackPresentationTest.cpp
    test/unit/app/TrackRowDataProviderTest.cpp
    test/unit/app/TrackListAdapterTest.cpp
    test/unit/app/SmartListEngineTest.cpp
    app/platform/linux/ui/TrackPresentation.cpp
    app/core/model/FilteredTrackIdList.cpp
    app/core/model/SmartListEngine.cpp
    app/core/model/TrackIdList.cpp
)
target_include_directories(rs_test PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/app)
target_link_libraries(rs_test PRIVATE rs Catch2)
if(FFmpeg_FOUND)
  target_sources(rs_test PRIVATE
    test/integration/playback/FfmpegDecoderSessionTest.cpp
    test/integration/tag/TagTest.cpp
    app/core/playback/FfmpegDecoderSession.cpp
    app/core/playback/MemoryPcmSource.cpp
    app/core/playback/PcmRingBuffer.cpp
    app/core/playback/StreamingPcmSource.cpp
  )
  target_link_libraries(rs_test PRIVATE FFmpeg)
  target_compile_definitions(rs_test PRIVATE FFmpeg_FOUND TAG_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/integration/tag/test_data")
endif()
target_compile_options(rs_test PRIVATE -Wno-array-bounds)

# Register rs_test with CTest
add_test(NAME rs_test COMMAND rs_test)

# Relaxed clang-tidy for test: allow magic numbers, chained comparisons
if(CLANG_TIDY_EXECUTABLE)
  set_target_properties(rs_test PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_RELAXED_CHECKS};-header-filter=${CLANG_TIDY_RELAXED_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
  )
endif()

add_executable(RockStudio
    app/platform/linux/main.cpp
    app/core/AppConfig.cpp
    app/platform/linux/ui/MainWindow.cpp
    app/platform/linux/ui/TrackPresentation.cpp
    app/platform/linux/ui/TrackViewPage.cpp
    app/platform/linux/ui/TrackListAdapter.cpp
    app/platform/linux/ui/TrackRow.cpp
    app/platform/linux/ui/ListRow.cpp
    app/platform/linux/ui/ListTreeNode.cpp
    app/core/ImportWorker.cpp
    app/platform/linux/services/PlaylistExporter.cpp
    app/platform/linux/ui/SmartListDialog.cpp
    app/platform/linux/ui/QueryExpressionBox.cpp
    app/platform/linux/ui/TagPromptDialog.cpp
    app/platform/linux/ui/TagPopover.cpp
    app/platform/linux/ui/ImportProgressDialog.cpp
    app/platform/linux/ui/StatusBar.cpp
    app/core/model/TrackIdList.cpp
    app/core/model/AllTrackIdsList.cpp
    app/core/model/FilteredTrackIdList.cpp
    app/core/model/SmartListEngine.cpp
    app/core/model/ManualTrackIdList.cpp
    app/core/model/TrackRowDataProvider.cpp
    app/platform/linux/ui/PlaybackBar.cpp
    app/core/playback/PlaybackController.cpp
    app/core/playback/PlaybackEngine.cpp
    app/core/playback/MemoryPcmSource.cpp
    app/core/playback/PcmRingBuffer.cpp
    app/core/playback/StreamingPcmSource.cpp
    app/core/playback/FormatNegotiator.cpp
)
if(FFmpeg_FOUND)
  target_compile_definitions(RockStudio PRIVATE FFmpeg_FOUND)
  target_sources(RockStudio PRIVATE app/core/playback/FfmpegDecoderSession.cpp)
  target_link_libraries(RockStudio PRIVATE FFmpeg)
endif()
if(PIPEWIRE_FOUND)
  target_compile_definitions(RockStudio PRIVATE PIPEWIRE_FOUND)
  target_sources(RockStudio PRIVATE app/platform/linux/playback/PipeWireBackend.cpp)
  target_link_libraries(RockStudio PRIVATE PipeWire)
endif()
if(ALSA_FOUND)
  target_compile_definitions(RockStudio PRIVATE ALSA_FOUND)
  target_sources(RockStudio PRIVATE app/platform/linux/playback/AlsaExclusiveBackend.cpp)
  target_link_libraries(RockStudio PRIVATE ALSA)
endif()
target_link_libraries(RockStudio PRIVATE rs GTKMM)
target_include_directories(RockStudio PRIVATE ${CMAKE_SOURCE_DIR}/app)
# Configure ASan leak detection to use suppression file for third-party libraries (RockStudio app only)
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
  set(ASAN_SUPPRESSIONS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/.asan_suppressions")
  # Use LSAN_OPTIONS to point to suppressions file
  set(ASAN_ENV "LSAN_OPTIONS=suppressions=${ASAN_SUPPRESSIONS_FILE}")
  
  # Apply to RockStudio for use with running via IDEs/ctest
  set_target_properties(RockStudio PROPERTIES ENVIRONMENT "${ASAN_ENV}")
endif()
# Note: clang-tidy disabled for RockStudio because GTK headers pull in GCC's SSE
# intrinsics (mmintrin.h) which clang cannot parse. Building with clang would enable tidy.
