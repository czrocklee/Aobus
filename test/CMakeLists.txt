add_executable(rs_test
    main.cpp
    unit/query/ParserTest.cpp
    unit/query/ExecutionPlanTest.cpp
    unit/query/PlanEvaluatorTest.cpp
    unit/core/TrackLayoutTest.cpp
    unit/core/TrackViewTest.cpp
    unit/core/TrackBuilderTest.cpp
    unit/core/ListLayoutTest.cpp
    unit/core/ListViewTest.cpp
    unit/core/DictionaryTest.cpp
    unit/core/ResourceStoreTest.cpp
    unit/core/TrackStoreTest.cpp
    unit/core/ListStoreTest.cpp
    unit/core/ListBuilderTest.cpp
    unit/core/MusicLibraryTest.cpp
    unit/core/LibraryExportImportTest.cpp
    unit/lmdb/EnvironmentTest.cpp
    unit/lmdb/TransactionTest.cpp
    unit/lmdb/DatabaseTest.cpp
    unit/app/TrackPresentationTest.cpp
    unit/app/TrackRowDataProviderTest.cpp
    unit/app/TrackListAdapterTest.cpp
    unit/app/SmartListEngineTest.cpp
    "${CMAKE_SOURCE_DIR}/app/core/playback/AudioDecoderSession.cpp"
    "${CMAKE_SOURCE_DIR}/app/core/Log.cpp"
    "${CMAKE_SOURCE_DIR}/app/platform/linux/ui/TrackPresentation.cpp"
    "${CMAKE_SOURCE_DIR}/app/core/model/FilteredTrackIdList.cpp"
    "${CMAKE_SOURCE_DIR}/app/core/model/SmartListEngine.cpp"
    "${CMAKE_SOURCE_DIR}/app/core/model/TrackIdList.cpp"
    "${CMAKE_SOURCE_DIR}/app/core/playback/FlacDecoderSession.cpp"
    "${CMAKE_SOURCE_DIR}/app/core/playback/AlacDecoderSession.cpp"
    "${CMAKE_SOURCE_DIR}/app/core/playback/MemoryPcmSource.cpp"
    "${CMAKE_SOURCE_DIR}/app/core/playback/PcmRingBuffer.cpp"
    "${CMAKE_SOURCE_DIR}/app/core/playback/StreamingPcmSource.cpp"
    integration/playback/DecoderIntegrationTest.cpp
    integration/tag/TagTest.cpp
)
target_include_directories(rs_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(rs_test PRIVATE rs Catch2 spdlog::spdlog GTKMM yaml-cpp PkgFLAC PkgALAC)
target_compile_definitions(rs_test PRIVATE TAG_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/integration/tag/test_data")

target_compile_options(rs_test PRIVATE -Wno-array-bounds)

# Register rs_test with CTest
add_test(NAME rs_test COMMAND rs_test)

# Relaxed clang-tidy for test: allow magic numbers, chained comparisons
if(CLANG_TIDY_EXECUTABLE)
  set_target_properties(rs_test PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_RELAXED_CHECKS};-header-filter=${CLANG_TIDY_RELAXED_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
  )
endif()
