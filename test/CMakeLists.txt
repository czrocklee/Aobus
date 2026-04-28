# rs_test: portable unit + integration tests (app_core only)
add_executable(rs_test
    main.cpp

    # query tests
    unit/query/ParserTest.cpp
    unit/query/ExecutionPlanTest.cpp
    unit/query/PlanEvaluatorTest.cpp

    # core tests
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

    # lmdb tests
    unit/lmdb/EnvironmentTest.cpp
    unit/lmdb/TransactionTest.cpp
    unit/lmdb/DatabaseTest.cpp

    # app unit tests (app_core only, no platform deps)
    unit/app/FormatNegotiatorTest.cpp
    unit/app/PlaybackControllerTest.cpp
    unit/app/PlaybackEngineTest.cpp
    unit/app/SmartListEngineTest.cpp

    # integration tests
    integration/playback/DecoderIntegrationTest.cpp
    integration/playback/PipeWireManagerTest.cpp
    integration/tag/TagTest.cpp
)

target_include_directories(rs_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/external")
target_link_libraries(rs_test PRIVATE rs Catch2 spdlog::spdlog yaml-cpp PkgFLAC PkgALAC app_core app_linux)

if(PIPEWIRE_FOUND)
  target_compile_definitions(rs_test PRIVATE PIPEWIRE_FOUND)
  target_link_libraries(rs_test PRIVATE PipeWire)
endif()

if(ALSA_FOUND)
  target_compile_definitions(rs_test PRIVATE ALSA_FOUND)
  target_link_libraries(rs_test PRIVATE ALSA UDEV)
endif()

target_compile_definitions(rs_test PRIVATE TAG_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/integration/tag/test_data")
target_compile_options(rs_test PRIVATE -Wno-array-bounds)
add_test(NAME rs_test COMMAND rs_test)

if(CLANG_TIDY_EXECUTABLE)
  set_target_properties(rs_test PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_RELAXED_CHECKS};-header-filter=${CLANG_TIDY_RELAXED_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
  )
endif()

# rs_test_linux: Linux UI tests (require app_linux and GTKMM)
add_executable(rs_test_linux
    main.cpp

    unit/linux/TrackListAdapterTest.cpp
    unit/linux/TrackPresentationTest.cpp
    unit/linux/TrackRowDataProviderTest.cpp
)

target_include_directories(rs_test_linux PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/external")
target_link_libraries(rs_test_linux PRIVATE rs Catch2 spdlog::spdlog app_core app_linux GTKMM)
add_test(NAME rs_test_linux COMMAND rs_test_linux)