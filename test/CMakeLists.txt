# ao_test: portable unit + integration tests
add_executable(ao_test
    # query tests
    unit/query/ParserTest.cpp
    unit/query/ExecutionPlanTest.cpp
    unit/query/PlanEvaluatorTest.cpp
    unit/query/ExpressionTest.cpp
    unit/query/SerializerTest.cpp

    # library tests
    unit/library/TrackLayoutTest.cpp
    unit/library/TrackViewTest.cpp
    unit/library/TrackBuilderTest.cpp
    unit/library/ListLayoutTest.cpp
    unit/library/ListViewTest.cpp
    unit/library/DictionaryStoreTest.cpp
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

    # audio unit tests
    unit/audio/DecoderFactoryTest.cpp
    unit/audio/QualityAnalyzerTest.cpp
    unit/audio/FormatNegotiatorTest.cpp
    unit/audio/MemorySourceTest.cpp
    unit/audio/StreamingSourceTest.cpp
    unit/audio/PcmRingBufferTest.cpp
    unit/audio/backend/detail/PipeWireSharedTest.cpp
    unit/audio/PcmConverterTest.cpp
    unit/audio/PlayerTest.cpp
    unit/audio/EngineTest.cpp
    unit/audio/backend/detail/PipeWireMonitorHelpersTest.cpp
    unit/audio/backend/detail/AlsaProviderHelpersTest.cpp

    # app unit tests (no platform deps)
    unit/runtime/SmartListEvaluatorTest.cpp
    unit/runtime/TrackPresentationPresetTest.cpp
    unit/runtime/TrackListProjectionTest.cpp
    unit/runtime/TrackListProjectionScaleTest.cpp
    unit/runtime/TrackDetailProjectionTest.cpp
    unit/runtime/LibraryMutationServiceTest.cpp
    unit/runtime/NotificationServiceTest.cpp
    unit/runtime/ViewServiceTest.cpp
    unit/runtime/ConfigStoreTest.cpp
    unit/runtime/ManualListSourceTest.cpp
    unit/runtime/HeadlessShellTest.cpp

    # integration tests
    integration/audio/DecoderIntegrationTest.cpp
    integration/audio/GraphIntegrityTest.cpp
    integration/audio/backend/PipeWireProviderTest.cpp
    integration/tag/TagTest.cpp
)

target_include_directories(ao_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/external")
target_link_libraries(ao_test PRIVATE ao ao_audio app_runtime Catch2 Catch2::Catch2WithMain spdlog::spdlog yaml-cpp PkgFLAC PkgALAC)

if(PIPEWIRE_FOUND)
  target_compile_definitions(ao_test PRIVATE PIPEWIRE_FOUND)
  target_link_libraries(ao_test PRIVATE PkgPipeWire)
endif()

if(ALSA_FOUND)
  target_compile_definitions(ao_test PRIVATE ALSA_FOUND)
  target_link_libraries(ao_test PRIVATE PkgALSA PkgUDEV)
endif()

target_compile_definitions(ao_test PRIVATE TAG_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/integration/tag/test_data")
target_compile_options(ao_test PRIVATE -Wno-array-bounds)
add_test(NAME ao_test COMMAND ao_test)
aobus_apply_clang_tidy(ao_test RELAXED)

# ao_test_gtk: Linux UI tests (require aobus-gtk-lib and GTKMM)
add_executable(ao_test_gtk
    unit/linux-gtk/track/TrackListAdapterTest.cpp
    unit/linux-gtk/track/TrackPresentationTest.cpp
    unit/linux-gtk/track/TrackRowCacheTest.cpp
    unit/linux-gtk/layout/document/LayoutModelTest.cpp
    unit/linux-gtk/layout/components/ContainersTest.cpp
    unit/linux-gtk/layout/components/LayoutComponentsTest.cpp
    unit/linux-gtk/layout/editor/LayoutEditorTest.cpp
    unit/linux-gtk/layout/components/StatusComponentsTest.cpp
)

target_include_directories(ao_test_gtk PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/external")
target_link_libraries(ao_test_gtk PRIVATE ao aobus-gtk-lib PkgGTKMM Catch2 spdlog::spdlog Catch2::Catch2WithMain)
add_test(NAME ao_test_gtk COMMAND ao_test_gtk)

# ao_perf_baseline: standalone performance baseline (not run by default in build.sh)
add_executable(ao_perf_baseline
    unit/runtime/PerformanceBaselineTest.cpp
)

target_include_directories(ao_perf_baseline PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/external")
target_link_libraries(ao_perf_baseline PRIVATE ao ao_audio app_runtime Catch2 Catch2::Catch2WithMain spdlog::spdlog yaml-cpp PkgFLAC PkgALAC)

if(PIPEWIRE_FOUND)
    target_compile_definitions(ao_perf_baseline PRIVATE PIPEWIRE_FOUND)
    target_link_libraries(ao_perf_baseline PRIVATE PkgPipeWire)
endif()

if(ALSA_FOUND)
    target_compile_definitions(ao_perf_baseline PRIVATE ALSA_FOUND)
    target_link_libraries(ao_perf_baseline PRIVATE PkgALSA PkgUDEV)
endif()

target_compile_definitions(ao_perf_baseline PRIVATE TAG_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/integration/tag/test_data")
target_compile_options(ao_perf_baseline PRIVATE -Wno-array-bounds)