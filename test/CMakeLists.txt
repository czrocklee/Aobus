# ao_test_options: shared test compilation settings
add_library(ao_test_options INTERFACE)
target_include_directories(ao_test_options INTERFACE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_compile_definitions(ao_test_options INTERFACE TAG_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/integration/tag/test_data")
target_compile_options(ao_test_options INTERFACE
  -Wno-array-bounds
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,15>>:-Wno-mismatched-new-delete>
)

if(PIPEWIRE_FOUND)
  target_compile_definitions(ao_test_options INTERFACE PIPEWIRE_FOUND)
  target_link_libraries(ao_test_options INTERFACE PkgPipeWire)
endif()

if(ALSA_FOUND)
  target_compile_definitions(ao_test_options INTERFACE ALSA_FOUND)
  target_link_libraries(ao_test_options INTERFACE PkgALSA PkgUDEV)
endif()

# ao_core_test: portable unit + integration tests
add_executable(ao_core_test
    # utility tests
    unit/utility/AtomicFileTest.cpp
    unit/utility/Base64Test.cpp
    unit/utility/MappedFileTest.cpp
    unit/utility/LogTest.cpp
    unit/utility/ByteViewTest.cpp
    unit/utility/StringArenaTest.cpp
    unit/utility/StrongTypeTest.cpp
    unit/core/AudioCodecTest.cpp
    unit/core/ExceptionTest.cpp
    unit/core/ErrorTest.cpp

    # query tests
    unit/query/CompletionBaselineTest.cpp
    unit/query/CompletionLogicalOperatorTest.cpp
    unit/query/CompletionOperatorTest.cpp
    unit/query/CompletionValueTest.cpp
    unit/query/CompletionVariableTest.cpp
    unit/query/CompletionTokenizerTest.cpp
    unit/query/ParserTest.cpp
    unit/query/PredicateTest.cpp
    unit/query/ExecutionPlanAccessProfileTest.cpp
    unit/query/ExecutionPlanCompileTest.cpp
    unit/query/ExecutionPlanContractTest.cpp
    unit/query/ExecutionPlanDictionaryTest.cpp
    unit/query/ExecutionPlanFieldCatalogTest.cpp
    unit/query/ExecutionPlanMembershipTest.cpp
    unit/query/ExecutionPlanStringExpressionTest.cpp
    unit/query/ExecutionPlanTagBloomTest.cpp
    unit/query/ExecutionPlanUnitLiteralTest.cpp
    unit/query/FormatExpressionTest.cpp
    unit/query/PlanEvaluatorCollectionTest.cpp
    unit/query/PlanEvaluatorComparisonTest.cpp
    unit/query/PlanEvaluatorDictionaryTest.cpp
    unit/query/PlanEvaluatorExecutionTest.cpp
    unit/query/PlanEvaluatorLogicalTest.cpp
    unit/query/PlanEvaluatorMediaPropertyTest.cpp
    unit/query/PlanEvaluatorStringTest.cpp
    unit/query/PlanEvaluatorTagTest.cpp
    unit/query/ExpressionTest.cpp
    unit/query/SerializerTest.cpp
    unit/query/QueryErrorTest.cpp

    # tag tests
    unit/tag/MpegFrameTest.cpp
    unit/tag/MpegFileTest.cpp
    unit/tag/Mp4FileTest.cpp
    unit/tag/id3v2/ReaderTest.cpp
    unit/tag/FlacFileTest.cpp
    unit/tag/TagFileTest.cpp
    unit/tag/DecoderTest.cpp
    unit/tag/TagErrorTest.cpp

    # media tests
    unit/media/mp4/DemuxerTest.cpp
    unit/media/mp4/SampleDescriptionTest.cpp

    # library tests
    unit/library/TrackLayoutTest.cpp
    unit/library/TrackViewTest.cpp
    unit/library/TrackViewTagTest.cpp
    unit/library/TrackViewCustomMetadataTest.cpp
    unit/library/TrackViewValidationTest.cpp
    unit/library/FileManifestViewTest.cpp
    unit/library/FileManifestStoreTest.cpp
    unit/library/FileManifestBuilderTest.cpp
    unit/library/TrackBuilderTest.cpp
    unit/library/TrackBuilderCoverArtTest.cpp
    unit/library/TrackBuilderTagTest.cpp
    unit/library/TrackBuilderSerializationTest.cpp
    unit/library/TrackBuilderOverflowTest.cpp
    unit/library/LibraryErrorTest.cpp
    unit/library/MetaStoreTest.cpp
    unit/library/ListLayoutTest.cpp
    unit/library/ListViewTest.cpp
    unit/library/DictionaryStoreTest.cpp
    unit/library/ScanPlanExecutorTest.cpp
    unit/library/LibraryScannerTest.cpp
    unit/library/ListStoreTest.cpp
    unit/library/ResourceStoreTest.cpp

    unit/library/TrackStoreTest.cpp
    unit/library/TrackStoreRawLayoutTest.cpp
    unit/library/ListBuilderTest.cpp
    unit/library/MusicLibraryTest.cpp

    # lmdb tests
    unit/lmdb/EnvironmentTest.cpp
    unit/lmdb/TransactionTest.cpp
    unit/lmdb/DatabaseTest.cpp
    unit/lmdb/DatabaseReaderTest.cpp
    unit/lmdb/DatabaseMaxKeyTest.cpp
    unit/lmdb/DatabaseWriterTest.cpp
    unit/lmdb/DatabaseBlobKeyTest.cpp
    unit/lmdb/ResultErrorTest.cpp

    # audio unit tests
    unit/audio/DecoderFactoryTest.cpp
    unit/audio/detail/AacConfigParserTest.cpp
    unit/audio/detail/MappedFileCursorTest.cpp
    unit/audio/detail/Mp4PacketSourceTest.cpp
    unit/audio/detail/TimeConversionTest.cpp
    unit/audio/DecoderErrorTest.cpp
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
    unit/audio/FormatTest.cpp
    unit/audio/StreamingSourceTest.cpp
    unit/audio/PcmRingBufferTest.cpp
    unit/audio/backend/detail/PipeWireSharedTest.cpp
    unit/audio/backend/PipeWireMonitorTest.cpp
    unit/audio/backend/detail/AlsaGraphRegistryTest.cpp
    unit/audio/backend/detail/AlsaPcmErrorTest.cpp
    unit/audio/backend/detail/AlsaPcmVolumeTest.cpp
    unit/audio/PcmConverterTest.cpp
    unit/audio/PlayerTest.cpp
    unit/audio/EngineTest.cpp
    unit/audio/EngineBackendSwapTest.cpp
    unit/audio/EngineCallbackTest.cpp
    unit/audio/EngineConcurrencyTest.cpp
    unit/audio/EnginePropertyTest.cpp
    unit/audio/EngineTransportTest.cpp
    unit/audio/backend/detail/PipeWireMonitorHelpersTest.cpp
    unit/audio/backend/detail/AlsaProviderHelpersTest.cpp

     # ui model tests (no platform deps)
    unit/uimodel/field/TrackFieldEditCodecTest.cpp
     unit/uimodel/field/TrackFieldEditPolicyTest.cpp
     unit/uimodel/field/TrackFieldFormatterTest.cpp
    unit/uimodel/field/TrackInlineEditWorkflowTest.cpp
    unit/uimodel/input/KeyChordTest.cpp
    unit/uimodel/input/KeymapModelTest.cpp
    unit/uimodel/input/KeymapStoreTest.cpp
    unit/uimodel/layout/action/LayoutActionCatalogTest.cpp
    unit/uimodel/layout/action/LayoutActionValidatorTest.cpp
    unit/uimodel/layout/component/AbsoluteCanvasModelTest.cpp
    unit/uimodel/layout/component/LayoutComponentCatalogTest.cpp
    unit/uimodel/layout/component/LayoutComponentStateTest.cpp
    unit/uimodel/layout/component/LayoutStatePromoterTest.cpp
    unit/uimodel/layout/document/LayoutModelTest.cpp
    unit/uimodel/layout/document/LayoutTemplateExpanderTest.cpp
    unit/uimodel/layout/shell/ShellLayoutSessionModelTest.cpp
    unit/uimodel/library/detail/TrackCustomPropertyWorkflowTest.cpp
    unit/uimodel/library/detail/TrackFieldGridPolicyTest.cpp
    unit/uimodel/library/detail/TrackFieldGridSchemaTest.cpp
    unit/uimodel/library/list/ListActionPolicyTest.cpp
    unit/uimodel/library/list/ListTreeProjectionTest.cpp
    unit/uimodel/library/list/SmartListEditorModelTest.cpp
    unit/uimodel/library/presentation/CustomPresentationEditorModelTest.cpp
    unit/uimodel/library/presentation/ListPresentationPreferenceStoreTest.cpp
    unit/uimodel/library/presentation/TrackColumnLayoutPolicyTest.cpp
    unit/uimodel/library/presentation/TrackColumnLayoutStoreTest.cpp
    unit/uimodel/library/presentation/TrackFieldPresentationPolicyTest.cpp
    unit/uimodel/library/presentation/TrackPresentationCatalogTest.cpp
    unit/uimodel/library/presentation/TrackPresentationPickerViewModelTest.cpp
    unit/uimodel/library/presentation/TrackPresentationRecommenderTest.cpp
    unit/uimodel/library/property/TagEditWorkflowTest.cpp
    unit/uimodel/library/property/TrackPropertiesFormSpecTest.cpp
    unit/uimodel/library/property/TrackPropertiesFormModelTest.cpp
    unit/uimodel/library/track/TrackFilterResolverTest.cpp
    unit/uimodel/library/track/TrackFilterViewModelTest.cpp
    unit/uimodel/library/track/TrackPageRouteTest.cpp
    unit/uimodel/library/track/TrackSelectionRegionPolicyTest.cpp
    unit/uimodel/library/track/TrackSelectionSummaryTest.cpp
    unit/uimodel/playback/now-playing/NowPlayingViewModelTest.cpp
    unit/uimodel/playback/output/OutputDeviceViewModelTest.cpp
    unit/uimodel/playback/output/VolumeViewModelTest.cpp
    unit/uimodel/playback/quality/AudioQualityFormatterTest.cpp
    unit/uimodel/playback/queue/PlaybackQueueModelTest.cpp
    unit/uimodel/playback/seek/PlaybackPositionInterpolatorTest.cpp
    unit/uimodel/playback/seek/PlaybackTimeViewModelTest.cpp
    unit/uimodel/playback/seek/SeekSliderInteractionModelTest.cpp
    unit/uimodel/playback/seek/SeekViewModelTest.cpp
    unit/uimodel/playback/soul/AobusSoulViewModelTest.cpp
    unit/uimodel/playback/transport/TransportViewModelTest.cpp
    unit/uimodel/status/activity/ActivityStatusModelCompactTest.cpp
    unit/uimodel/status/activity/ActivityStatusModelDetailTest.cpp
    unit/uimodel/status/activity/ActivityStatusModelNotificationTest.cpp
    unit/uimodel/status/activity/ActivityStatusModelPresentationTest.cpp

    # app unit tests (no platform deps)
    unit/runtime/AsyncRuntimeTest.cpp
    unit/runtime/QueuedExecutorTest.cpp
    unit/runtime/library/LibraryReaderTest.cpp
    unit/runtime/StorageResultTest.cpp
    unit/runtime/completion/CompletionServiceTest.cpp
    unit/runtime/LifetimeScopeTest.cpp
    unit/runtime/source/SmartListEvaluatorBatchTest.cpp
    unit/runtime/source/SmartListEvaluatorExpressionTest.cpp
    unit/runtime/source/SmartListEvaluatorIncrementalTest.cpp
    unit/runtime/source/SmartListEvaluatorLoadModeTest.cpp
    unit/runtime/source/SmartListEvaluatorTest.cpp
    unit/runtime/TrackPresentationTest.cpp
    unit/runtime/library/LibraryWriterTagCommandTest.cpp
    unit/runtime/library/LibraryWriterTrackCreateCommandTest.cpp
    unit/runtime/library/LibraryWriterTrackDeleteCommandTest.cpp
    unit/runtime/TrackFieldTest.cpp
    unit/runtime/projection/TrackListProjectionGroupingTest.cpp
    unit/runtime/projection/TrackListProjectionLifecycleTest.cpp
    unit/runtime/projection/TrackListProjectionMutationTest.cpp
    unit/runtime/projection/TrackListProjectionSortingTest.cpp
    unit/runtime/projection/TrackListProjectionScaleTest.cpp
    unit/runtime/PlaybackServiceControlTest.cpp
    unit/runtime/PlaybackServiceOutputTest.cpp
    unit/runtime/PlaybackServiceSelectionTest.cpp
    unit/runtime/PlaybackServiceTest.cpp
    unit/runtime/source/AllTracksSourceTest.cpp
    unit/runtime/source/ListSourceStoreTest.cpp
    unit/runtime/projection/TrackDetailProjectionTest.cpp
    unit/runtime/library/LibraryTasksTest.cpp
    unit/runtime/library/LibraryWriterTest.cpp
    unit/runtime/completion/MetadataValueCompleterTest.cpp
    unit/runtime/NotificationServiceTest.cpp
    unit/runtime/completion/QueryExpressionCompleterTest.cpp
    unit/runtime/ViewServiceLifecycleTest.cpp
    unit/runtime/ViewServiceListFilterTest.cpp
    unit/runtime/ViewServicePresentationTest.cpp
    unit/runtime/ViewServiceSelectionTest.cpp
    unit/runtime/ConfigStoreTest.cpp
    unit/runtime/source/ManualListSourceObserverTest.cpp
    unit/runtime/source/ManualListSourceTest.cpp
    unit/runtime/NavigationHistoryTest.cpp
    unit/runtime/WorkspaceHistoryTest.cpp
    unit/runtime/WorkspaceNavigationTest.cpp
    unit/runtime/WorkspacePresentationTest.cpp
    unit/runtime/WorkspaceSessionTest.cpp
    unit/runtime/AppRuntimeTest.cpp
    unit/runtime/HeadlessShellTest.cpp
    unit/runtime/library/LibraryExportImportCoverArtTest.cpp
    unit/runtime/library/LibraryExportImportDeltaTest.cpp
    unit/runtime/library/LibraryExportImportErrorTest.cpp
    unit/runtime/library/LibraryExportImportListTest.cpp
    unit/runtime/library/LibraryExportImportTest.cpp
)

target_link_libraries(ao_core_test PRIVATE ao_app_uimodel ao_test_options Catch2::Catch2WithMain FakeIt::FakeIt-catch)
add_test(NAME ao_core_test COMMAND ao_core_test)

# ao_tui_test: terminal frontend unit tests
add_executable(ao_tui_test
    unit/tui/CoverArtTest.cpp
    unit/tui/EventControllerTest.cpp
    unit/tui/ExecutorTest.cpp
    unit/tui/LibraryControllerTest.cpp
    unit/tui/ModelTest.cpp
    unit/tui/OutputDeviceControllerTest.cpp
    unit/tui/PlaybackActionsTest.cpp
    unit/tui/PlaybackPanelTest.cpp
    unit/tui/RenderTest.cpp
    unit/tui/ShellModelTest.cpp
    unit/tui/TrackTableTest.cpp
)

target_link_libraries(ao_tui_test PRIVATE aobus-tui-lib ao_app_uimodel ao_app_runtime ao_test_options Catch2::Catch2WithMain FakeIt::FakeIt-catch)
add_test(NAME ao_tui_test COMMAND ao_tui_test)

# ao_fleet_test: fleet tool unit and integration tests
add_executable(ao_fleet_test
    unit/fleet/ModelTest.cpp
    unit/fleet/IntentSerializationTest.cpp
    unit/fleet/PatchGuardTest.cpp
    unit/fleet/RegistrySerializationTest.cpp
    unit/fleet/RouteStoreTest.cpp
    unit/fleet/SchedulerTest.cpp
    unit/fleet/SubstrateTest.cpp
    unit/fleet/YamlStreamSerializationTest.cpp
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

target_link_libraries(ao_integration_test PRIVATE ao_app_uimodel ao_test_options Catch2::Catch2WithMain FakeIt::FakeIt-catch)

# Not using add_test() here to keep it out of default 'ctest' runs, 
# or use labels/properties if preferred. For now, it's just a manual driver.


# ao_gtk_test: Linux UI tests (require aobus-gtk-lib and GTKMM)
add_executable(ao_gtk_test
    unit/linux-gtk/common/DismissControllerTest.cpp
    unit/linux-gtk/common/UiWorkflowTest.cpp
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
    unit/linux-gtk/image/ImageRenderPolicyTest.cpp
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
    unit/linux-gtk/app/LibraryWindowPolicyTest.cpp
    unit/linux-gtk/app/KeyboardShortcutsWindowTest.cpp
    unit/linux-gtk/app/MouseNavigationPolicyTest.cpp
    unit/linux-gtk/app/AobusSoulTest.cpp
    unit/linux-gtk/app/GtkStyleRuntimeTest.cpp
    unit/linux-gtk/app/GtkMainContextExecutorTest.cpp
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
    unit/linux-gtk/status/ActivityStatusTest.cpp
    unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp
    unit/linux-gtk/portal/LibraryImportExportWorkflowTest.cpp
    unit/linux-gtk/platform/AudioBackendBootstrapTest.cpp
      unit/linux-gtk/layout/document/LayoutModelTest.cpp
      unit/linux-gtk/layout/runtime/ActionRegistryTest.cpp
      unit/linux-gtk/layout/runtime/LayoutContextTest.cpp
      unit/linux-gtk/layout/runtime/ActionBinderTest.cpp
      unit/linux-gtk/layout/runtime/ActionValidatorTest.cpp
      unit/linux-gtk/layout/runtime/ComponentInteractionControllerTest.cpp
      unit/linux-gtk/layout/runtime/ComponentRegistryInjectionTest.cpp
      unit/linux-gtk/layout/runtime/GioActionBridgeTest.cpp
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
      unit/linux-gtk/layout/components/PlaybackLayoutComponentsTest.cpp
      unit/linux-gtk/layout/components/SemanticLayoutComponentsTest.cpp
      unit/linux-gtk/layout/components/LayoutComponentYamlTest.cpp
      unit/linux-gtk/layout/components/TrackFieldGridCollapsibleTest.cpp
      unit/linux-gtk/layout/components/TrackDetailConstrainedLayoutTest.cpp
      unit/linux-gtk/layout/components/TrackFieldGridTextUtilsTest.cpp
      unit/linux-gtk/layout/components/PlaybackImageTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorAbsoluteCanvasTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorDescriptorTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorDialogSessionTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorTemplateTest.cpp
      unit/linux-gtk/layout/components/StatusComponentsTest.cpp
    unit/linux-gtk/playback/AudioPipelinePanelTest.cpp
    unit/linux-gtk/playback/AobusSoulWindowTest.cpp
    unit/linux-gtk/playback/OutputDeviceSelectorTest.cpp
    unit/linux-gtk/playback/PlaybackUiComponentsTest.cpp
    unit/linux-gtk/playback/PlaybackDetailsWidgetTest.cpp
    unit/linux-gtk/playback/NowPlayingFieldLabelTest.cpp
    unit/linux-gtk/playback/NowPlayingStatusLabelTest.cpp
    unit/linux-gtk/playback/OutputDeviceListItemsTest.cpp
    unit/linux-gtk/playback/TransportButtonTest.cpp
    unit/linux-gtk/playback/VolumeControlTest.cpp
)

target_link_libraries(ao_gtk_test PRIVATE aobus-gtk-lib PkgGTKMM ao_test_options Catch2::Catch2WithMain FakeIt::FakeIt-catch)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(ao_gtk_test PRIVATE -Wl,-u,aobus_get_resource)
endif()
add_test(NAME ao_gtk_test COMMAND ao_gtk_test)

# ao_perf_baseline: standalone performance baseline (not run by default in ./ao check)
add_executable(ao_perf_baseline
    perf/PerformanceBaselineTest.cpp
)

target_link_libraries(ao_perf_baseline PRIVATE ao_app_uimodel ao_test_options Catch2::Catch2WithMain FakeIt::FakeIt-catch)

# Compilation database support for custom linter integration fixture
file(GLOB_RECURSE LINT_FIXTURE_SOURCES "integration/lint/fixture/*.cpp")
add_library(ao_lint_fixture OBJECT EXCLUDE_FROM_ALL ${LINT_FIXTURE_SOURCES})
target_include_directories(ao_lint_fixture PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/integration/lint/fixture")
