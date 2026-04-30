# rs_test: portable unit + integration tests
add_executable(rs_test
    # query tests
    unit/query/ParserTest.cpp
    unit/query/ExecutionPlanTest.cpp
    unit/query/PlanEvaluatorTest.cpp

    # library tests
    unit/library/TrackLayoutTest.cpp
    unit/library/TrackViewTest.cpp
    unit/library/TrackBuilderTest.cpp
    unit/library/ListLayoutTest.cpp
    unit/library/ListViewTest.cpp
    unit/library/DictionaryTest.cpp
    unit/library/ResourceStoreTest.cpp
    unit/library/TrackStoreTest.cpp
    unit/library/ListStoreTest.cpp
    unit/library/ListBuilderTest.cpp
    unit/library/MusicLibraryTest.cpp
    unit/library/LibraryExportImportTest.cpp

    # lmdb tests
    unit/lmdb/EnvironmentTest.cpp
    unit/lmdb/TransactionTest.cpp
    unit/lmdb/DatabaseTest.cpp

    # app unit tests (no platform deps)
    unit/app/FormatNegotiatorTest.cpp
    unit/app/PlaybackControllerTest.cpp
    unit/app/PlaybackEngineTest.cpp
    unit/app/SmartListEngineTest.cpp

    # integration tests
    unit/query/ParserTest.cpp
    integration/playback/DecoderIntegrationTest.cpp
    integration/playback/PipeWireManagerTest.cpp
    integration/playback/GraphIntegrityTest.cpp
    integration/tag/TagTest.cpp
)

target_include_directories(rs_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/external")
target_link_libraries(rs_test PRIVATE rs app_linux Catch2 Catch2::Catch2WithMain spdlog::spdlog yaml-cpp PkgFLAC PkgALAC)

if(PIPEWIRE_FOUND)
  target_compile_definitions(rs_test PRIVATE PIPEWIRE_FOUND)
  target_link_libraries(rs_test PRIVATE PkgPipeWire)
endif()

if(ALSA_FOUND)
  target_compile_definitions(rs_test PRIVATE ALSA_FOUND)
  target_link_libraries(rs_test PRIVATE PkgALSA PkgUDEV)
endif()

target_compile_definitions(rs_test PRIVATE TAG_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/integration/tag/test_data")
target_compile_options(rs_test PRIVATE -Wno-array-bounds)
add_test(NAME rs_test COMMAND rs_test)
rockstudio_apply_clang_tidy(rs_test RELAXED)

# rs_test_linux: Linux UI tests (require app_linux and GTKMM)
add_executable(rs_test_linux
    unit/linux/TrackListAdapterTest.cpp
    unit/linux/TrackPresentationTest.cpp
    unit/linux/TrackRowDataProviderTest.cpp
)

target_include_directories(rs_test_linux PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/external")
target_link_libraries(rs_test_linux PRIVATE rs app_linux PkgGTKMM Catch2 spdlog::spdlog Catch2::Catch2WithMain)
add_test(NAME rs_test_linux COMMAND rs_test_linux)