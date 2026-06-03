# ao_test: portable unit + integration tests
add_executable(ao_test
    # utility tests
    unit/utility/Base64Test.cpp
    unit/utility/MappedFileTest.cpp
    unit/utility/LogTest.cpp
    unit/utility/ByteViewTest.cpp
    unit/core/ExceptionTest.cpp
    unit/core/ErrorTest.cpp

    # query tests
    unit/query/ParserTest.cpp
    unit/query/ExecutionPlanTest.cpp
    unit/query/PlanEvaluatorTest.cpp
    unit/query/ExpressionTest.cpp
    unit/query/SerializerTest.cpp

    # tag tests
    unit/tag/MpegFrameTest.cpp
    unit/tag/MpegFileTest.cpp
    unit/tag/Mp4FileTest.cpp
    unit/tag/id3v2/ReaderTest.cpp
    unit/tag/FlacFileTest.cpp
    unit/tag/TagFileTest.cpp

    # media tests
    unit/media/mp4/DemuxerTest.cpp

    # library tests
    unit/library/TrackLayoutTest.cpp
    unit/library/TrackViewTest.cpp
    unit/library/FileManifestViewTest.cpp
    unit/library/FileManifestStoreTest.cpp
    unit/library/FileManifestBuilderTest.cpp
    unit/library/TrackBuilderTest.cpp
    unit/library/MetaStoreTest.cpp
    unit/library/ListLayoutTest.cpp
    unit/library/ListViewTest.cpp
    unit/library/DictionaryStoreTest.cpp
    unit/library/ScanPlanExecutorTest.cpp
    unit/library/LibraryScannerTest.cpp
    unit/library/ListStoreTest.cpp

    unit/library/TrackStoreTest.cpp
    unit/library/ListStoreTest.cpp
    unit/library/ListBuilderTest.cpp
    unit/library/MusicLibraryTest.cpp

    # lmdb tests
    unit/lmdb/EnvironmentTest.cpp
    unit/lmdb/TransactionTest.cpp
    unit/lmdb/DatabaseTest.cpp

    # audio unit tests
    unit/audio/DecoderFactoryTest.cpp
    unit/audio/FlacDecoderSessionTest.cpp
    unit/audio/AlacDecoderSessionTest.cpp
    unit/audio/QualityAnalyzerTest.cpp
    unit/audio/FormatNegotiatorTest.cpp
    unit/audio/MemorySourceTest.cpp
    unit/audio/StreamingSourceTest.cpp
    unit/audio/PcmRingBufferTest.cpp
    unit/audio/backend/detail/PipeWireSharedTest.cpp
    unit/audio/backend/detail/AlsaGraphRegistryTest.cpp
    unit/audio/backend/detail/AlsaPcmVolumeTest.cpp
    unit/audio/PcmConverterTest.cpp
    unit/audio/PlayerTest.cpp
    unit/audio/EngineTest.cpp
    unit/audio/backend/detail/PipeWireMonitorHelpersTest.cpp
    unit/audio/backend/detail/AlsaProviderHelpersTest.cpp

    # ui model tests (no platform deps)
    unit/uimodel/layout/ActionCatalogTest.cpp
    unit/uimodel/layout/ActionValidatorTest.cpp
    unit/uimodel/layout/ComponentCatalogTest.cpp
    unit/uimodel/layout/LayoutModelTest.cpp
    unit/uimodel/layout/LayoutTemplateExpanderTest.cpp
    unit/uimodel/list/ListActionPolicyTest.cpp
    unit/uimodel/list/SmartListEditorModelTest.cpp
    unit/uimodel/playback/AobusSoulViewModelTest.cpp
    unit/uimodel/playback/AudioOutputViewModelTest.cpp
    unit/uimodel/playback/AudioQualityFormatTest.cpp
    unit/uimodel/playback/NowPlayingViewModelTest.cpp
    unit/uimodel/playback/PlaybackQueueModelTest.cpp
    unit/uimodel/playback/PlaybackPositionInterpolatorTest.cpp
    unit/uimodel/playback/PlaybackTimeViewModelTest.cpp
    unit/uimodel/playback/SeekViewModelTest.cpp
    unit/uimodel/playback/TransportViewModelTest.cpp
    unit/uimodel/playback/VolumeViewModelTest.cpp
    unit/uimodel/tag/TagEditWorkflowTest.cpp
    unit/uimodel/track/TrackFilterViewModelTest.cpp
    unit/uimodel/track/TrackFilterResolverTest.cpp
    unit/uimodel/track/TrackPageRouteTest.cpp
    unit/uimodel/track/TrackFieldFormatterTest.cpp
    unit/uimodel/track/TrackPresentationViewModelTest.cpp
    unit/uimodel/track/TrackPresentationRecommenderTest.cpp

    # app unit tests (no platform deps)
    unit/runtime/AsyncRuntimeTest.cpp
    unit/runtime/LifetimeScopeTest.cpp
    unit/runtime/SmartListEvaluatorTest.cpp
    unit/runtime/TrackPresentationTest.cpp
    unit/runtime/TrackCommandServiceTest.cpp
    unit/runtime/TrackFieldTest.cpp
    unit/runtime/TrackListProjectionTest.cpp
    unit/runtime/TrackListProjectionScaleTest.cpp
    unit/runtime/PlaybackServiceTest.cpp
  unit/runtime/AllTracksSourceTest.cpp
  unit/runtime/ListSourceStoreTest.cpp
    unit/runtime/TrackDetailProjectionTest.cpp
    unit/runtime/LibraryMutationServiceTest.cpp
    unit/runtime/NotificationServiceTest.cpp
    unit/runtime/ViewServiceTest.cpp
    unit/runtime/ConfigStoreTest.cpp
    unit/runtime/ManualListSourceTest.cpp
    unit/runtime/NavigationHistoryTest.cpp
    unit/runtime/WorkspaceNavigationTest.cpp
    unit/runtime/AppRuntimeTest.cpp
    unit/runtime/HeadlessShellTest.cpp
    unit/runtime/LibraryExportImportTest.cpp
)

target_include_directories(ao_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_test PRIVATE ao ao_audio ao_app_uimodel ao_app_runtime Catch2 Catch2::Catch2WithMain FakeIt::FakeIt-catch spdlog::spdlog PkgRapidYaml PkgFLAC PkgALAC)

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

# ao_integration_test: standalone integration tests (excluded from default build.sh runs)
add_executable(ao_integration_test
    integration/audio/DecoderIntegrationTest.cpp
    integration/audio/GraphIntegrityTest.cpp
    integration/audio/backend/PipeWireProviderTest.cpp
    integration/tag/TagTest.cpp
)

target_include_directories(ao_integration_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_integration_test PRIVATE ao ao_audio ao_app_uimodel ao_app_runtime Catch2 Catch2::Catch2WithMain FakeIt::FakeIt-catch spdlog::spdlog PkgRapidYaml PkgFLAC PkgALAC)

if(PIPEWIRE_FOUND)
  target_compile_definitions(ao_integration_test PRIVATE PIPEWIRE_FOUND)
  target_link_libraries(ao_integration_test PRIVATE PkgPipeWire)
endif()

if(ALSA_FOUND)
  target_compile_definitions(ao_integration_test PRIVATE ALSA_FOUND)
  target_link_libraries(ao_integration_test PRIVATE PkgALSA PkgUDEV)
endif()

target_compile_definitions(ao_integration_test PRIVATE TAG_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/integration/tag/test_data")
target_compile_options(ao_integration_test PRIVATE -Wno-array-bounds)

# Not using add_test() here to keep it out of default 'ctest' runs, 
# or use labels/properties if preferred. For now, it's just a manual driver.


# ao_test_gtk: Linux UI tests (require aobus-gtk-lib and GTKMM)
add_executable(ao_test_gtk
    unit/linux-gtk/list/ListModelTest.cpp
    unit/linux-gtk/list/ListTreeModelBuilderTest.cpp
    unit/linux-gtk/list/QueryExpressionBoxTest.cpp
    unit/linux-gtk/list/SmartListDialogTest.cpp
    unit/linux-gtk/list/ListNavigationControllerTest.cpp
    unit/linux-gtk/tag/TagEditorTest.cpp
    unit/linux-gtk/tag/TagEditControllerTest.cpp
    unit/linux-gtk/tag/TagPopoverTest.cpp
    unit/linux-gtk/tag/TrackPropertiesDialogTest.cpp
    unit/linux-gtk/image/ImageWidgetTest.cpp
    unit/linux-gtk/image/ImageCacheTest.cpp
    unit/linux-gtk/app/AppDialogTest.cpp
    unit/linux-gtk/app/ThemeCoordinatorTest.cpp
    unit/linux-gtk/app/ThemePresetTest.cpp
    unit/linux-gtk/app/MainWindowCoordinatorTest.cpp
    unit/linux-gtk/app/AobusSoulTest.cpp
    unit/linux-gtk/app/GtkStyleRuntimeTest.cpp
    unit/linux-gtk/app/AppConfigTest.cpp
    unit/linux-gtk/app/ShellLayoutControllerTest.cpp
    unit/linux-gtk/app/GtkLayoutConfigTest.cpp
    unit/linux-gtk/track/TrackListModelTest.cpp
    unit/linux-gtk/track/TrackSelectionControllerTest.cpp
    unit/linux-gtk/track/TrackPageHostTest.cpp
    unit/linux-gtk/track/TrackViewPageTest.cpp
    unit/linux-gtk/track/TrackColumnControllerTest.cpp
    unit/linux-gtk/track/TrackColumnFactoryBuilderTest.cpp
    unit/linux-gtk/track/TrackQuickFilterTest.cpp
    unit/linux-gtk/track/TrackPresentationButtonTest.cpp
    unit/linux-gtk/track/TrackCustomViewDialogTest.cpp
    unit/linux-gtk/track/TrackPresentationTest.cpp
    unit/linux-gtk/track/TrackFieldUiTest.cpp
    unit/linux-gtk/track/TrackRowCacheTest.cpp
      unit/linux-gtk/layout/document/LayoutModelTest.cpp
      unit/linux-gtk/layout/runtime/ActionRegistryTest.cpp
      unit/linux-gtk/layout/runtime/ActionBinderTest.cpp
      unit/linux-gtk/layout/runtime/ActionValidatorTest.cpp
      unit/linux-gtk/layout/runtime/ComponentInteractionControllerTest.cpp
      unit/linux-gtk/layout/runtime/ComponentRegistryInjectionTest.cpp
      unit/linux-gtk/layout/runtime/GioActionBridgeTest.cpp
      unit/linux-gtk/layout/components/ContainersTest.cpp
      unit/linux-gtk/layout/components/LayoutComponentsTest.cpp
      unit/linux-gtk/layout/components/TrackDetailConstrainedLayoutTest.cpp
      unit/linux-gtk/layout/components/PlaybackImageTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorTest.cpp
      unit/linux-gtk/layout/components/StatusComponentsTest.cpp
      unit/linux-gtk/app/AppConfigTest.cpp
    unit/linux-gtk/playback/AudioPipelinePanelTest.cpp
    unit/linux-gtk/playback/PlaybackUiComponentsTest.cpp
    unit/linux-gtk/playback/PlaybackDetailsWidgetTest.cpp
    unit/linux-gtk/playback/NowPlayingFieldLabelTest.cpp
    unit/linux-gtk/playback/NowPlayingStatusLabelTest.cpp
    unit/linux-gtk/playback/OutputListItemsTest.cpp
    unit/linux-gtk/playback/TransportButtonTest.cpp
    unit/linux-gtk/playback/TimeLabelTest.cpp
)

target_include_directories(ao_test_gtk PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_test_gtk PRIVATE aobus-gtk-lib)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(ao_test_gtk PRIVATE -Wl,-u,aobus_get_resource)
endif()

target_link_libraries(ao_test_gtk PRIVATE ao PkgGTKMM Catch2 FakeIt::FakeIt-catch spdlog::spdlog Catch2::Catch2WithMain)
add_test(NAME ao_test_gtk COMMAND ao_test_gtk)

# ao_perf_baseline: standalone performance baseline (not run by default in build.sh)
add_executable(ao_perf_baseline
    unit/runtime/PerformanceBaselineTest.cpp
)

target_include_directories(ao_perf_baseline PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_perf_baseline PRIVATE ao ao_audio ao_app_uimodel ao_app_runtime Catch2 Catch2::Catch2WithMain FakeIt::FakeIt-catch spdlog::spdlog PkgRapidYaml PkgFLAC PkgALAC)

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

# Compilation database support for custom linter integration fixture
add_library(ao_lint_fixture OBJECT EXCLUDE_FROM_ALL 
    integration/lint/fixture/BracedInitializationCheckFixture.cpp
    integration/lint/fixture/CApiGlobalQualificationCheckFixture.cpp
    integration/lint/fixture/ConcreteFinalCheckFixture.cpp
    integration/lint/fixture/IncludeConventionCheckFixture.cpp
    integration/lint/fixture/NodiscardUsageCheckFixture.cpp
    integration/lint/fixture/ForbidTrailingReturnCheckFixture.cpp
    integration/lint/fixture/IdentifierNamingExtensionsCheckFixture.cpp
    integration/lint/fixture/LambdaParamsCheckFixture.cpp
    integration/lint/fixture/LocalInitializationStyleCheckFixture.cpp
    integration/lint/fixture/MemberOrderCheckFixture.cpp
    integration/lint/fixture/OptionalNamingAndUsageCheckFixture.cpp
    integration/lint/fixture/StdCLibraryQualificationCheckFixture.cpp
    integration/lint/fixture/ThreadingPolicyCheckFixture.cpp
    integration/lint/fixture/UnusedSuppressionStyleCheckFixture.cpp
    integration/lint/fixture/UseIfInitStatementCheckFixture.cpp
)
target_include_directories(ao_lint_fixture PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/integration/lint/fixture")
