# ao_core_test: portable unit + integration tests
add_executable(ao_core_test
    # utility tests
    unit/utility/AtomicFileTest.cpp
    unit/utility/Base64Test.cpp
    unit/utility/MappedFileTest.cpp
    unit/utility/LogTest.cpp
    unit/utility/ByteViewTest.cpp
    unit/utility/StringArenaTest.cpp
    unit/core/ExceptionTest.cpp
    unit/core/ErrorTest.cpp

    # query tests
    unit/query/CompletionTest.cpp
    unit/query/CompletionTokenizerTest.cpp
    unit/query/ParserTest.cpp
    unit/query/PredicateTest.cpp
    unit/query/ExecutionPlanTest.cpp
    unit/query/FormatExpressionTest.cpp
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
    unit/media/mp4/SampleDescriptionTest.cpp

    # library tests
    unit/library/AudioCodecTest.cpp
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
    unit/library/ResourceStoreTest.cpp

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
    unit/audio/detail/AacConfigParserTest.cpp
    unit/audio/detail/MappedFileCursorTest.cpp
    unit/audio/detail/Mp4PacketSourceTest.cpp
    unit/audio/detail/TimeConversionTest.cpp
    unit/audio/AacDecoderSessionTest.cpp
    unit/audio/AacDecoderMalformedMp4Test.cpp
    unit/audio/FlacDecoderSessionTest.cpp
    unit/audio/FlacDecoderMalformedTest.cpp
    unit/audio/AlacDecoderSessionTest.cpp
    unit/audio/AlacDecoderMalformedMp4Test.cpp
    unit/audio/Mp3DecoderSessionTest.cpp
    unit/audio/Mp3DecoderMalformedTest.cpp
    unit/audio/QualityAnalyzerTest.cpp
    unit/audio/FormatNegotiatorTest.cpp
    unit/audio/MemorySourceTest.cpp
    unit/audio/StreamingSourceTest.cpp
    unit/audio/PcmRingBufferTest.cpp
    unit/audio/backend/detail/PipeWireSharedTest.cpp
    unit/audio/backend/PipeWireMonitorTest.cpp
    unit/audio/backend/detail/AlsaGraphRegistryTest.cpp
    unit/audio/backend/detail/AlsaPcmVolumeTest.cpp
    unit/audio/PcmConverterTest.cpp
    unit/audio/PlayerTest.cpp
    unit/audio/EngineTest.cpp
    unit/audio/backend/detail/PipeWireMonitorHelpersTest.cpp
    unit/audio/backend/detail/AlsaProviderHelpersTest.cpp

    # ui model tests (no platform deps)
    unit/uimodel/input/KeyChordTest.cpp
    unit/uimodel/input/KeymapModelTest.cpp
    unit/uimodel/input/KeymapStoreTest.cpp
    unit/uimodel/layout/AbsoluteCanvasModelTest.cpp
    unit/uimodel/layout/ActionCatalogTest.cpp
    unit/uimodel/layout/ActionValidatorTest.cpp
    unit/uimodel/layout/ComponentCatalogTest.cpp
    unit/uimodel/layout/LayoutModelTest.cpp
    unit/uimodel/layout/ShellLayoutSessionModelTest.cpp
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
    unit/uimodel/playback/SeekSliderInteractionModelTest.cpp
    unit/uimodel/playback/SeekViewModelTest.cpp
    unit/uimodel/playback/TransportViewModelTest.cpp
    unit/uimodel/playback/VolumeViewModelTest.cpp
    unit/uimodel/status/StatusSlotModelTest.cpp
    unit/uimodel/tag/TagEditWorkflowTest.cpp
    unit/uimodel/track/TrackFilterViewModelTest.cpp
    unit/uimodel/track/TrackFilterResolverTest.cpp
    unit/uimodel/track/TrackPageRouteTest.cpp
    unit/uimodel/track/TrackCustomViewEditorModelTest.cpp
    unit/uimodel/track/TrackFieldFormatterTest.cpp
    unit/uimodel/track/TrackInlineEditWorkflowTest.cpp
    unit/uimodel/track/SelectionSummaryTest.cpp
    unit/uimodel/track/TrackPresentationViewModelTest.cpp
    unit/uimodel/track/TrackPresentationRecommenderTest.cpp

    # app unit tests (no platform deps)
    unit/runtime/AsyncRuntimeTest.cpp
    unit/runtime/CompletionServiceTest.cpp
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
    unit/runtime/MetadataValueCompleterTest.cpp
    unit/runtime/NotificationServiceTest.cpp
    unit/runtime/QueryExpressionCompleterTest.cpp
    unit/runtime/ViewServiceTest.cpp
    unit/runtime/ConfigStoreTest.cpp
    unit/runtime/ManualListSourceTest.cpp
    unit/runtime/NavigationHistoryTest.cpp
    unit/runtime/WorkspaceNavigationTest.cpp
    unit/runtime/AppRuntimeTest.cpp
    unit/runtime/HeadlessShellTest.cpp
    unit/runtime/LibraryExportImportTest.cpp
)

target_include_directories(ao_core_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_core_test PRIVATE ao ao_audio ao_app_uimodel ao_app_runtime Catch2 Catch2::Catch2WithMain FakeIt::FakeIt-catch spdlog::spdlog PkgRapidYaml PkgFLAC PkgALAC PkgFDKAAC PkgMPG123)

if(PIPEWIRE_FOUND)
  target_compile_definitions(ao_core_test PRIVATE PIPEWIRE_FOUND)
  target_link_libraries(ao_core_test PRIVATE PkgPipeWire)
endif()

if(ALSA_FOUND)
  target_compile_definitions(ao_core_test PRIVATE ALSA_FOUND)
  target_link_libraries(ao_core_test PRIVATE PkgALSA PkgUDEV)
endif()

target_compile_definitions(ao_core_test PRIVATE TAG_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/integration/tag/test_data")
target_compile_options(ao_core_test PRIVATE
  -Wno-array-bounds
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,15>>:-Wno-mismatched-new-delete>
)
add_test(NAME ao_core_test COMMAND ao_core_test)

# ao_fleet_test: fleet tool unit and integration tests
add_executable(ao_fleet_test
    unit/fleet/ModelTest.cpp
    unit/fleet/PatchGuardTest.cpp
    unit/fleet/RouteStoreTest.cpp
    unit/fleet/SchedulerTest.cpp
    unit/fleet/SerializationTest.cpp
    unit/fleet/SubstrateTest.cpp
    integration/fleet/FleetRunnerTest.cpp
    integration/fleet/ProcessRunnerTest.cpp
    integration/fleet/SubstrateTest.cpp
)
target_include_directories(ao_fleet_test PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ao_fleet_test PRIVATE ao_fleet Catch2::Catch2WithMain)
target_compile_definitions(ao_fleet_test PRIVATE AOBUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ao_fleet_test COMMAND ao_fleet_test)

# ao_integration_test: standalone integration tests (full gate only, not the default ./ao test loop)
add_executable(ao_integration_test
    integration/audio/DecoderIntegrationTest.cpp
    integration/audio/GraphIntegrityTest.cpp
    integration/audio/backend/PipeWireProviderTest.cpp
    integration/tag/TagTest.cpp
)

target_include_directories(ao_integration_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_integration_test PRIVATE ao ao_audio ao_app_uimodel ao_app_runtime Catch2 Catch2::Catch2WithMain FakeIt::FakeIt-catch spdlog::spdlog PkgRapidYaml PkgFLAC PkgALAC PkgFDKAAC PkgMPG123)

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


# ao_gtk_test: Linux UI tests (require aobus-gtk-lib and GTKMM)
add_executable(ao_gtk_test
    unit/linux-gtk/common/DismissControllerTest.cpp
    unit/linux-gtk/completion/EntryCompletionControllerTest.cpp
    unit/linux-gtk/list/ListModelTest.cpp
    unit/linux-gtk/list/ListTreeModelBuilderTest.cpp
    unit/linux-gtk/list/QueryExpressionBoxTest.cpp
    unit/linux-gtk/list/SmartListDialogTest.cpp
    unit/linux-gtk/list/ListNavigationControllerTest.cpp
    unit/linux-gtk/tag/TagEditorTest.cpp
    unit/linux-gtk/tag/TagEditControllerTest.cpp
    unit/linux-gtk/tag/TrackPropertiesDialogTest.cpp
    unit/linux-gtk/image/ImageWidgetTest.cpp
    unit/linux-gtk/image/ImageCacheTest.cpp
    unit/linux-gtk/image/ThumbnailLoaderTest.cpp
    unit/linux-gtk/app/AppDialogTest.cpp
    unit/linux-gtk/app/ThemeCoordinatorTest.cpp
    unit/linux-gtk/app/ThemePresetTest.cpp
    unit/linux-gtk/app/MainWindowCoordinatorTest.cpp
    unit/linux-gtk/app/MenuControllerTest.cpp
    unit/linux-gtk/app/PlaybackShortcutPolicyTest.cpp
    unit/linux-gtk/app/GtkAccelTranslatorTest.cpp
    unit/linux-gtk/app/KeymapApplicatorTest.cpp
    unit/linux-gtk/app/KeyboardShortcutsWindowTest.cpp
    unit/linux-gtk/app/MouseNavigationPolicyTest.cpp
    unit/linux-gtk/app/AobusSoulTest.cpp
    unit/linux-gtk/app/GtkStyleRuntimeTest.cpp
    unit/linux-gtk/app/AppConfigTest.cpp
    unit/linux-gtk/app/MainWindowTest.cpp
    unit/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp
    unit/linux-gtk/app/ShellLayoutStoreTest.cpp
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
    unit/linux-gtk/track/TrackScrollBaselineTest.cpp
    unit/linux-gtk/track/SelectionInfoLabelTest.cpp
    unit/linux-gtk/track/StatusSlotTest.cpp
    unit/linux-gtk/portal/LibraryTaskProgressDialogTest.cpp
    unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp
    unit/linux-gtk/platform/AudioBackendBootstrapTest.cpp
      unit/linux-gtk/layout/document/LayoutModelTest.cpp
      unit/linux-gtk/layout/runtime/ActionRegistryTest.cpp
      unit/linux-gtk/layout/runtime/LayoutContextTest.cpp
      unit/linux-gtk/layout/runtime/ActionBinderTest.cpp
      unit/linux-gtk/layout/runtime/ActionValidatorTest.cpp
      unit/linux-gtk/layout/runtime/ComponentInteractionControllerTest.cpp
      unit/linux-gtk/layout/runtime/ComponentRegistryInjectionTest.cpp
      unit/linux-gtk/layout/runtime/GioActionBridgeTest.cpp
      unit/linux-gtk/layout/state/LayoutComponentStateTest.cpp
      unit/linux-gtk/layout/state/LayoutStatePromoterTest.cpp
      unit/linux-gtk/layout/components/LayoutRuntimeBuildTest.cpp
      unit/linux-gtk/layout/components/ContainerErrorStatesTest.cpp
      unit/linux-gtk/layout/components/SplitComponentTest.cpp
      unit/linux-gtk/layout/components/CollapsibleSplitComponentTest.cpp
      unit/linux-gtk/layout/components/CenterBoxTest.cpp
      unit/linux-gtk/layout/components/ResponsiveClassTest.cpp
      unit/linux-gtk/layout/components/ScrollSeparatorTabsTest.cpp
      unit/linux-gtk/layout/components/ApplyCommonPropsTest.cpp
      unit/linux-gtk/layout/components/LayoutHostTest.cpp
      unit/linux-gtk/layout/components/LayoutComponentsTest.cpp
      unit/linux-gtk/layout/components/TrackFieldGridCollapsibleTest.cpp
      unit/linux-gtk/layout/components/TrackDetailConstrainedLayoutTest.cpp
      unit/linux-gtk/layout/components/TrackFieldGridTextUtilsTest.cpp
      unit/linux-gtk/layout/components/PlaybackImageTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorTest.cpp
      unit/linux-gtk/layout/components/StatusComponentsTest.cpp
      unit/linux-gtk/app/AppConfigTest.cpp
    unit/linux-gtk/playback/AudioPipelinePanelTest.cpp
    unit/linux-gtk/playback/AobusSoulWindowTest.cpp
    unit/linux-gtk/playback/AudioDeviceSelectorTest.cpp
    unit/linux-gtk/playback/PlaybackUiComponentsTest.cpp
    unit/linux-gtk/playback/PlaybackDetailsWidgetTest.cpp
    unit/linux-gtk/playback/NowPlayingFieldLabelTest.cpp
    unit/linux-gtk/playback/NowPlayingStatusLabelTest.cpp
    unit/linux-gtk/playback/OutputListItemsTest.cpp
    unit/linux-gtk/playback/TransportButtonTest.cpp
    unit/linux-gtk/playback/TimeLabelTest.cpp
)

target_include_directories(ao_gtk_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_gtk_test PRIVATE aobus-gtk-lib)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(ao_gtk_test PRIVATE -Wl,-u,aobus_get_resource)
endif()

target_link_libraries(ao_gtk_test PRIVATE ao PkgGTKMM Catch2 FakeIt::FakeIt-catch spdlog::spdlog Catch2::Catch2WithMain)
add_test(NAME ao_gtk_test COMMAND ao_gtk_test)

# ao_perf_baseline: standalone performance baseline (not run by default in ./ao check)
add_executable(ao_perf_baseline
    unit/runtime/PerformanceBaselineTest.cpp
)

target_include_directories(ao_perf_baseline PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_perf_baseline PRIVATE ao ao_audio ao_app_uimodel ao_app_runtime Catch2 Catch2::Catch2WithMain FakeIt::FakeIt-catch spdlog::spdlog PkgRapidYaml PkgFLAC PkgALAC PkgFDKAAC PkgMPG123)

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
file(GLOB_RECURSE LINT_FIXTURE_SOURCES "integration/lint/fixture/*.cpp")
add_library(ao_lint_fixture OBJECT EXCLUDE_FROM_ALL ${LINT_FIXTURE_SOURCES})
target_include_directories(ao_lint_fixture PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/integration/lint/fixture")
