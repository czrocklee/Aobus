# ao_test_options: shared test compilation settings
add_library(ao_test_options INTERFACE)
target_include_directories(ao_test_options INTERFACE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_compile_definitions(ao_test_options INTERFACE AUDIO_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/data/audio")
target_compile_options(ao_test_options INTERFACE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wno-array-bounds>
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,15>>:-Wno-mismatched-new-delete>
)

if(LINUX)
  target_link_libraries(ao_test_options INTERFACE PkgPipeWire PkgALSA PkgUDEV)
endif()

# Concrete test support is compiled once per layer. Keep these libraries
# dependency-directed: a narrow test must not inherit runtime or frontend
# headers merely to use a temporary directory or a Track fixture.
add_library(ao_test_support_common STATIC
    unit/TestFixtureSupport.cpp
)
target_link_libraries(ao_test_support_common
    PUBLIC
      ao_test_options
      ao_utility
      Catch2::Catch2
)

add_library(ao_test_support_library STATIC
    unit/library/LibraryBinaryTestSupport.cpp
    unit/library/LibraryStoreTestSupport.cpp
    unit/library/MusicLibraryTestSupport.cpp
    unit/library/TrackBuilderTestSupport.cpp
    unit/library/TrackStoreTestSupport.cpp
    unit/library/TrackTestSupport.cpp
    unit/library/TrackViewTestSupport.cpp
    unit/library/WritableLibraryTestSupport.cpp
)
target_link_libraries(ao_test_support_library
    PUBLIC
      ao_test_support_common
      ao_library
      Catch2::Catch2
)

add_library(ao_test_support_audio STATIC
    unit/audio/AudioFixtureSupport.cpp
    unit/audio/BackendTestSupport.cpp
    unit/audio/DecoderTestSupport.cpp
    unit/audio/EngineTestSupport.cpp
    unit/audio/FakeCapturingBackend.cpp
    unit/audio/ScriptedDecoderSession.cpp
)
target_link_libraries(ao_test_support_audio
    PUBLIC
      ao_test_options
      ao_audio
      FakeIt::FakeIt-catch
    PRIVATE
      ao_test_support_common
      Catch2::Catch2
)

add_library(ao_test_support_runtime STATIC
    unit/runtime/AppRuntimeTestSupport.cpp
    unit/runtime/AsyncTestSupport.cpp
    unit/runtime/ExecutorTestSupport.cpp
    unit/runtime/PlaybackSuccessionBaseTestSupport.cpp
    unit/runtime/PlaybackSuccessionSeekTestSupport.cpp
    unit/runtime/PlaybackSuccessionTransportTestSupport.cpp
    unit/runtime/PlaybackTestSupport.cpp
    unit/runtime/PlaybackTransportTestSupport.cpp
    unit/runtime/PlaybackUiTestSupport.cpp
    unit/runtime/RuntimeLibraryTestSupport.cpp
    unit/runtime/ViewServiceTestSupport.cpp
    unit/runtime/WorkspaceTestSupport.cpp
    unit/runtime/projection/TrackListProjectionTestSupport.cpp
    unit/runtime/source/ListOrderSourceTestSupport.cpp
    unit/runtime/source/SmartListEvaluatorTestSupport.cpp
    unit/runtime/source/TrackSourceTestSupport.cpp
)
target_link_libraries(ao_test_support_runtime
    PUBLIC
      ao_test_options
      ao_test_support_audio
      ao_test_support_library
      ao_test_support_common
      ao_app_runtime
      Catch2::Catch2
      FakeIt::FakeIt-catch
)

if(AOBUS_BUILD_GTK)
  add_library(ao_test_support_gtk STATIC
      unit/linux-gtk/GtkApplicationTestSupport.cpp
      unit/linux-gtk/GtkLayoutTestSupport.cpp
      unit/linux-gtk/GtkRuntimeTestSupport.cpp
      unit/linux-gtk/GtkWidgetTestSupport.cpp
      unit/linux-gtk/image/ImageTestSupport.cpp
      unit/linux-gtk/layout/LayoutTestSupport.cpp
      unit/linux-gtk/layout/components/ContainerTestHelpers.cpp
      unit/linux-gtk/layout/state/FakeLayoutComponentStateStore.cpp
  )
  target_link_libraries(ao_test_support_gtk
      PUBLIC
        ao_test_options
        aobus-gtk-lib
        PkgGTKMM
      PRIVATE
        ao_test_support_runtime
        ao_test_support_library
        ao_test_support_common
        Catch2::Catch2
  )
endif()

# ao_core_test: shared and host-native unit + integration tests
add_executable(ao_core_test
    # async tests
    unit/async/RequestCoalescerTest.cpp
    unit/async/SignalTest.cpp

    # utility tests
    unit/utility/AtomicFileTest.cpp
    unit/utility/Base64Test.cpp
    unit/utility/Xxh3Test.cpp
    unit/utility/MappedFileTest.cpp
    unit/utility/PathTest.cpp
    unit/utility/ByteViewTest.cpp
    unit/utility/StringArenaTest.cpp
    unit/utility/StrongTypeTest.cpp
    unit/utility/UuidTest.cpp
    unit/utility/YamlReflectTest.cpp
    unit/utility/RymlAdapterTest.cpp
    unit/utility/ScopedRegistrationTest.cpp
    unit/utility/YamlSerializationTest.cpp
    unit/utility/TestFixtureSupportTest.cpp
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
    unit/query/ExecutionPlanTestSupport.cpp
    unit/query/ExecutionPlanMembershipTest.cpp
    unit/query/ExecutionPlanStringExpressionTest.cpp
    unit/query/ExecutionPlanTagBloomTest.cpp
    unit/query/ExecutionPlanUnitLiteralTest.cpp
    unit/query/FormatExpressionTest.cpp
    unit/query/PlanEvaluatorTestSupport.cpp
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

    # media file tests
    unit/media/file/TestFile.cpp
    unit/media/file/MpegFrameTest.cpp
    unit/media/file/MpegFileTest.cpp
    unit/media/file/Mp4FileTest.cpp
    unit/media/file/id3v2/ReaderTest.cpp
    unit/media/file/FlacFileTest.cpp
    unit/media/file/WavFileTest.cpp
    unit/media/file/FileTest.cpp
    unit/media/file/DecoderTest.cpp

    # media tests
    unit/media/mp4/TestAtoms.cpp
    unit/media/mp4/AtomTest.cpp
    unit/media/mp4/DemuxerTest.cpp
    unit/media/mp4/SampleDescriptionTest.cpp
    unit/media/wav/TestWav.cpp
    unit/media/wav/RiffTest.cpp

    # library tests
    unit/library/TrackLayoutTest.cpp
    unit/library/TrackViewTest.cpp
    unit/library/TrackViewTagTest.cpp
    unit/library/TrackViewCustomMetadataTest.cpp
    unit/library/TrackViewValidationTest.cpp
    unit/library/FileManifestViewTest.cpp
    unit/library/FileManifestStoreTest.cpp
    unit/library/LibraryUriTest.cpp
    unit/library/FileManifestBuilderTest.cpp
    unit/library/TrackBuilderTest.cpp
    unit/library/TrackBuilderCoverArtTest.cpp
    unit/library/TrackBuilderTagTest.cpp
    unit/library/TrackBuilderSerializationTest.cpp
    unit/library/TrackBuilderOverflowTest.cpp
    unit/library/LibraryErrorTest.cpp
    unit/library/MetadataStoreTest.cpp
    unit/library/ListLayoutTest.cpp
    unit/library/ListViewTest.cpp
    unit/library/DictionaryStoreTest.cpp
    unit/library/AudioIdentityTest.cpp
    unit/library/ListStoreTest.cpp
    unit/library/ResourceStoreTest.cpp

    unit/library/TrackStoreTest.cpp
    unit/library/TrackWriteTest.cpp
    unit/library/TrackStoreRawLayoutTest.cpp
    unit/library/TrackStoreIntegrityTest.cpp
    unit/library/ListBuilderTest.cpp
    unit/library/MusicLibraryTest.cpp

    # lmdb tests
    unit/lmdb/LmdbTestSupport.cpp
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
    unit/audio/DecoderOutputTest.cpp
    unit/audio/OpenedModeValidationTest.cpp
    unit/audio/detail/DecoderOutputAdapterTest.cpp
    unit/audio/NullBackendTest.cpp
    unit/audio/detail/AacConfigParserTest.cpp
    unit/audio/detail/MappedFileCursorTest.cpp
    unit/audio/detail/Mp4PacketSourceTest.cpp
    unit/audio/detail/TimeConversionTest.cpp
    unit/audio/detail/StreamingBufferPolicyTest.cpp
    unit/audio/DecoderErrorTest.cpp
    unit/audio/AacDecoderSessionTest.cpp
    unit/audio/AacDecoderMalformedMp4Test.cpp
    unit/audio/FlacDecoderSessionTest.cpp
    unit/audio/FlacDecoderMalformedTest.cpp
    unit/audio/AlacDecoderSessionTest.cpp
    unit/audio/AlacDecoderMalformedMp4Test.cpp
    unit/audio/Mp3DecoderSessionTest.cpp
    unit/audio/WavDecoderSessionTest.cpp
    unit/audio/Mp3DecoderMalformedTest.cpp
    unit/audio/QualityAnalyzerTest.cpp
    unit/audio/FormatTest.cpp
    unit/audio/StreamingSourceTest.cpp
    unit/audio/PcmRingBufferTest.cpp
    unit/audio/backend/detail/AudioBackendRenderProgressTest.cpp
    unit/audio/PcmConversionTest.cpp
    unit/audio/PlayerTest.cpp
    unit/audio/EngineDrainTest.cpp
    unit/audio/EngineErrorTest.cpp
    unit/audio/EngineGaplessTest.cpp
    unit/audio/EngineTest.cpp
    unit/audio/EngineBackendSwapTest.cpp
    unit/audio/EngineCallbackTest.cpp
    unit/audio/EngineConcurrencyTest.cpp
    unit/audio/EnginePropertyTest.cpp
    unit/audio/EngineTransportTest.cpp

     # ui model tests (no platform deps)
    unit/uimodel/field/TrackFieldEditCodecTest.cpp
     unit/uimodel/field/TrackFieldEditPolicyTest.cpp
     unit/uimodel/field/TrackFieldFormatterTest.cpp
    unit/uimodel/input/KeyChordTest.cpp
    unit/uimodel/input/KeymapModelTest.cpp
    unit/uimodel/input/KeyRepeatGuardTest.cpp
    unit/uimodel/input/KeymapStoreTest.cpp
    unit/uimodel/preference/PreferencesEditorModelTest.cpp
    unit/uimodel/preference/ThemePresetTest.cpp
    unit/uimodel/layout/action/LayoutActionCatalogTest.cpp
    unit/uimodel/layout/action/LayoutActionSlotResolutionTest.cpp
    unit/uimodel/layout/action/LayoutActionValidatorTest.cpp
    unit/uimodel/layout/component/AbsoluteCanvasGeometryTest.cpp
    unit/uimodel/layout/component/LayoutComponentCatalogTest.cpp
    unit/uimodel/layout/component/LayoutComponentStateTest.cpp
    unit/uimodel/layout/component/LayoutStatePromoterTest.cpp
    unit/uimodel/layout/component/StatefulComponentStateTest.cpp
    unit/uimodel/layout/document/LayoutModelTest.cpp
    unit/uimodel/layout/document/LayoutPlacementTest.cpp
    unit/uimodel/layout/document/LayoutPreparationTest.cpp
    unit/uimodel/layout/document/LayoutValidationTest.cpp
    unit/uimodel/layout/document/LayoutTemplateExpansionTest.cpp
    unit/uimodel/layout/shell/LayoutBuildStateViewTest.cpp
    unit/uimodel/layout/shell/ShellGenerationSequenceTest.cpp
    unit/uimodel/layout/shell/ShellLayoutSessionModelTest.cpp
    unit/uimodel/presentation/CoverArtPlaceholderTest.cpp
    unit/uimodel/presentation/PresentationTextCatalogTest.cpp
    unit/uimodel/library/detail/TrackCustomMetadataTest.cpp
    unit/uimodel/library/detail/TrackFieldGridPolicyTest.cpp
    unit/uimodel/library/detail/TrackFieldGridSchemaTest.cpp
    unit/uimodel/library/list/ListActionPolicyTest.cpp
    unit/uimodel/library/list/ListMembershipAuthoringSessionTest.cpp
    unit/uimodel/library/list/ListOrderAuthoringSessionTest.cpp
    unit/uimodel/library/list/ListOrderPolicyTest.cpp
    unit/uimodel/library/list/ListTreeProjectionTest.cpp
    unit/uimodel/library/list/SmartListEditorModelTest.cpp
    unit/uimodel/library/list/SmartListTrackPresentationResolverTest.cpp
    unit/uimodel/library/presentation/CustomPresentationEditorModelTest.cpp
    unit/uimodel/library/presentation/ListPresentationPreferenceYamlSchemaTest.cpp
    unit/uimodel/library/presentation/ListPresentationPreferenceStoreTest.cpp
    unit/uimodel/library/presentation/TrackColumnLayoutYamlSchemaTest.cpp
    unit/uimodel/library/presentation/TrackColumnLayoutPolicyTest.cpp
    unit/uimodel/library/presentation/TrackColumnLayoutStoreTest.cpp
    unit/uimodel/library/presentation/TrackColumnWidthSolverTest.cpp
    unit/uimodel/library/presentation/TrackFieldPresentationPolicyTest.cpp
    unit/uimodel/library/presentation/TrackPresentationCatalogTest.cpp
    unit/uimodel/library/presentation/TrackPresentationPickerViewModelTest.cpp
    unit/uimodel/library/presentation/TrackPresentationRecommenderTest.cpp
    unit/uimodel/library/property/TagEditTest.cpp
    unit/uimodel/library/property/TrackAuthoringSessionTest.cpp
    unit/uimodel/library/property/TrackAuthoringTestSupport.cpp
    unit/uimodel/library/property/TrackPropertiesFormSpecTest.cpp
    unit/uimodel/library/property/TrackPropertiesFormModelTest.cpp
    unit/uimodel/library/task/LibraryScanWorkflowTest.cpp
    unit/uimodel/library/track/IndexedTrackRowCacheTest.cpp
    unit/uimodel/library/track/TrackDisplayIndexTest.cpp
    unit/uimodel/library/track/TrackCountFormatterTest.cpp
    unit/uimodel/library/track/TrackFilterCompleterTest.cpp
    unit/uimodel/library/track/TrackFilterResolverTest.cpp
    unit/uimodel/library/track/TrackFilterViewModelTest.cpp
    unit/uimodel/library/track/TrackPageRouteTest.cpp
    unit/uimodel/library/track/TrackSelectionRegionPolicyTest.cpp
    unit/uimodel/library/track/TrackSelectionSummaryTest.cpp
    unit/uimodel/playback/now-playing/NowPlayingViewModelTest.cpp
    unit/uimodel/playback/command/PlaybackCommandSurfaceTest.cpp
    unit/uimodel/playback/output/OutputDeviceViewModelTest.cpp
    unit/uimodel/playback/output/VolumeViewModelTest.cpp
    unit/uimodel/playback/quality/AudioQualityFormatterTest.cpp
    unit/uimodel/playback/seek/PlaybackPositionInterpolatorTest.cpp
    unit/uimodel/playback/seek/PlaybackPositionViewModelTest.cpp
    unit/uimodel/playback/seek/PlaybackTimeFormatterTest.cpp
    unit/uimodel/playback/seek/SeekSliderInteractionModelTest.cpp
    unit/uimodel/playback/soul/AobusSoulViewModelTest.cpp
    unit/uimodel/playback/transport/TransportViewModelTest.cpp
    unit/uimodel/status/activity/ActivityStatusFeedProjectionTestSupport.cpp
    unit/uimodel/status/activity/ActivityStatusFeedProjectionCompactTest.cpp
    unit/uimodel/status/activity/ActivityStatusFeedProjectionDetailTest.cpp
    unit/uimodel/status/activity/ActivityStatusFeedProjectionNotificationTest.cpp
    unit/uimodel/status/activity/ActivityStatusViewModelTest.cpp

    # app unit tests (no platform deps)
    unit/runtime/AsyncParallelTest.cpp
    unit/runtime/AsyncRuntimeTest.cpp
    unit/runtime/ExecutorTestSupportTest.cpp
    unit/runtime/library/LibraryPathsTest.cpp
    unit/runtime/library/LibraryReaderTest.cpp
    unit/runtime/library/MediaTrackTest.cpp
    unit/runtime/completion/CompletionServiceTest.cpp
    unit/runtime/LifetimeScopeTest.cpp
    unit/runtime/LogTest.cpp
    unit/runtime/LoopExecutorTest.cpp
    unit/runtime/source/SmartListEvaluatorBatchTest.cpp
    unit/runtime/source/SmartListEvaluatorExpressionTest.cpp
    unit/runtime/source/SmartListEvaluatorIncrementalTest.cpp
    unit/runtime/source/SmartListEvaluatorLoadModeTest.cpp
    unit/runtime/source/SmartListEvaluatorTest.cpp
    unit/runtime/source/SourcePipelineOperationTest.cpp
    unit/runtime/source/IndexedTrackSequenceTest.cpp
    unit/runtime/source/SourcePipelineOracleTest.cpp
    unit/runtime/source/TrackSourceDeltaTest.cpp
    unit/runtime/source/TrackSourceDeltaBuilderTest.cpp
    unit/runtime/source/TrackSourceLifecycleTest.cpp
    unit/runtime/TrackPresentationTest.cpp
    unit/runtime/TrackEditScriptTest.cpp
    unit/runtime/library/LibraryWriterTagCommandTest.cpp
    unit/runtime/library/LibraryWriterTrackCreateCommandTest.cpp
    unit/runtime/library/LibraryWriterTrackDeleteCommandTest.cpp
    unit/runtime/library/LibraryWriterListMembershipTest.cpp
    unit/runtime/library/LibraryWriterListOrderTest.cpp
    unit/runtime/TrackFieldTest.cpp
    unit/runtime/library/LibraryWriterDryRunTest.cpp
    unit/runtime/library/LibraryChangesTest.cpp
    unit/runtime/projection/TrackListProjectionGroupingTest.cpp
    unit/runtime/projection/TrackListProjectionDeltaContractTest.cpp
    unit/runtime/projection/TrackListProjectionIncrementalTest.cpp
    unit/runtime/projection/TrackListProjectionLifecycleTest.cpp
    unit/runtime/projection/TrackListProjectionMutationTest.cpp
    unit/runtime/projection/TrackListProjectionSortingTest.cpp
    unit/runtime/projection/TrackListProjectionScaleTest.cpp
    unit/runtime/playback/PlaybackCursorModelTest.cpp
    unit/runtime/playback/PlaybackRestartDeadlineTest.cpp
    unit/runtime/playback/PreparedNextRegistryTest.cpp
    unit/runtime/playback/ProjectionAnchorTest.cpp
    unit/runtime/playback/ShuffleHistoryTest.cpp
    unit/runtime/PlaybackServiceTest.cpp
    unit/runtime/PlaybackTransportControlTest.cpp
    unit/runtime/PlaybackTransportOutputTest.cpp
    unit/runtime/PlaybackTransportSelectionTest.cpp
    unit/runtime/PlaybackTransportTest.cpp
    unit/runtime/PlaybackTransportTokenTest.cpp
    unit/runtime/PlaybackSuccessionLaunchTest.cpp
    unit/runtime/PlaybackSuccessionProjectionTest.cpp
    unit/runtime/PlaybackSuccessionAdvanceTest.cpp
    unit/runtime/PlaybackSuccessionFailureTest.cpp
    unit/runtime/PlaybackSessionTest.cpp
    unit/runtime/source/AllTracksSourceTest.cpp
    unit/runtime/source/TrackSourceCacheTest.cpp
    unit/runtime/source/VirtualListIdsTest.cpp
    unit/runtime/projection/TrackDetailProjectionTest.cpp
    unit/runtime/library/AudioIdentityIndexerTest.cpp
    unit/runtime/library/LibraryScanTest.cpp
    unit/runtime/library/ScanPlanTest.cpp
    unit/runtime/library/ScanApplyOperationTest.cpp
    unit/runtime/library/LibraryTaskServiceTest.cpp
    unit/runtime/library/LibraryWriterTest.cpp
    unit/runtime/library/LibraryAuthoringTest.cpp
    unit/runtime/completion/MetadataValueCompleterTest.cpp
    unit/runtime/NotificationServiceTest.cpp
    unit/runtime/NotificationServiceExpiryTest.cpp
    unit/runtime/WritableTagListTest.cpp
    unit/runtime/completion/QueryExpressionCompleterTest.cpp
    unit/runtime/ViewServiceLifecycleTest.cpp
    unit/runtime/ViewServiceListFilterTest.cpp
    unit/runtime/ViewServicePresentationTest.cpp
    unit/runtime/ViewServiceSelectionTest.cpp
    unit/runtime/ConfigStoreTest.cpp
    unit/runtime/source/ListOrderSourceObserverTest.cpp
    unit/runtime/source/ListOrderSourceTest.cpp
    unit/runtime/NavigationHistoryTest.cpp
    unit/runtime/WorkspaceHistoryTest.cpp
    unit/runtime/WorkspaceNavigationTest.cpp
    unit/runtime/WorkspacePresentationTest.cpp
    unit/runtime/WorkspaceSessionYamlSchemaTest.cpp
    unit/runtime/WorkspaceSessionTest.cpp
    unit/runtime/AppRuntimeTest.cpp
    unit/runtime/HeadlessShellTest.cpp
    unit/runtime/library/LibraryExportImportCoverArtTest.cpp
    unit/runtime/library/LibraryExportImportDeltaTest.cpp
    unit/runtime/library/LibraryExportImportErrorTest.cpp
    unit/runtime/library/LibraryExportImportListTest.cpp
    unit/runtime/library/LibraryYamlSchemaTest.cpp
    unit/runtime/library/LibraryExportImportTest.cpp
    unit/runtime/resource/ResourceByteCacheTest.cpp
    unit/runtime/resource/ResourceByteLoaderTest.cpp
)

target_link_libraries(ao_core_test PRIVATE
    ao_app_uimodel
    ao_test_support_runtime
    ao_test_options
    Catch2::Catch2WithMain
    FakeIt::FakeIt-catch
)
if(LINUX)
  target_sources(ao_core_test PRIVATE
    unit/FilesystemTestSupportPosix.cpp
    unit/audio/backend/PipeWireMonitorTest.cpp
    unit/audio/backend/AlsaExclusiveBackendTest.cpp
    unit/audio/backend/detail/PipeWireMonitorParsingTest.cpp
    unit/audio/backend/detail/AlsaGraphRegistryTest.cpp
    unit/audio/backend/detail/AlsaModeSelectorTest.cpp
    unit/audio/backend/detail/AlsaPcmErrorTest.cpp
    unit/audio/backend/detail/AlsaPcmFormatTest.cpp
    unit/audio/backend/detail/AlsaPcmVolumeTest.cpp
  )
endif()
if(WIN32)
  target_sources(ao_core_test PRIVATE
    unit/FilesystemTestSupportWindows.cpp
    unit/audio/backend/WasapiProviderTest.cpp
    unit/audio/backend/WasapiSharedBackendTest.cpp
    unit/audio/backend/detail/WasapiGraphRegistryTest.cpp
    unit/audio/backend/detail/WasapiStringsTest.cpp
    unit/windows/platform/MemoryRandomAccessStreamTest.cpp

    # Windows-owned shell rules are tested only by the native Windows suite.
    # The normal Windows test graph uses Ninja while aobus-winui-lib requires
    # the Visual Studio/XAML graph, so this target compiles the pure rule sources
    # directly instead of exporting a second cross-platform frontend library.
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/DestructiveLibraryRestart.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/DesktopSettingsYamlSchema.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/LibraryStartupPlan.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/StartupOptions.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/CommandLine.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/platform/WinUiErrorBoundary.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ElementKind.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/LayoutCatalog.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/LayoutDialect.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/PlacementPlan.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ShellDocument.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ShellStatePolicy.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/StyleLookup.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ThemeSurface.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/theme/Theme.cpp
    unit/winui/app/DestructiveLibraryRestartTest.cpp
    unit/winui/app/LibraryStartupPlanTest.cpp
    unit/winui/app/StartupOptionsTest.cpp
    unit/winui/app/CommandLineTest.cpp
    unit/winui/DesktopSettingsYamlSchemaTest.cpp
    unit/winui/FrameResourceTest.cpp
    unit/winui/StringResourceTest.cpp
    unit/winui/ThemeTest.cpp
    unit/winui/WinUiErrorBoundaryTest.cpp
    unit/winui/layout/ElementKindTest.cpp
    unit/winui/layout/LayoutCatalogTest.cpp
    unit/winui/layout/LayoutDialectTest.cpp
    unit/winui/layout/PlacementPlanTest.cpp
    unit/winui/layout/ShellDocumentTest.cpp
    unit/winui/layout/ShellStatePolicyTest.cpp
    unit/winui/layout/StyleLookupTest.cpp
    unit/winui/layout/ThemeSurfaceTest.cpp
  )
  target_include_directories(ao_core_test PRIVATE
    "${CMAKE_SOURCE_DIR}/app/windows-winui/include")
  target_compile_definitions(ao_core_test PRIVATE
    AOBUS_WINDOWS_LAYOUT_DIR="${CMAKE_SOURCE_DIR}/app/windows-winui/layout"
    AOBUS_WINDOWS_FRAME_XAML="${CMAKE_SOURCE_DIR}/app/windows-winui/MainWindow.xaml"
    AOBUS_WINDOWS_WINUI_CMAKE="${CMAKE_SOURCE_DIR}/app/windows-winui/CMakeLists.txt"
    AOBUS_WINDOWS_SHELL_BUILDER_SOURCE="${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ShellBuilder.cpp"
    AOBUS_WINDOWS_WINUI_DIR="${CMAKE_SOURCE_DIR}/app/windows-winui"
    AOBUS_WINDOWS_RESW="${CMAKE_SOURCE_DIR}/app/windows-winui/Resources/en-US/Resources.resw")
  target_link_libraries(ao_core_test PRIVATE advapi32 ao_windows_platform PkgRapidYaml)
endif()
add_test(NAME ao_core_test COMMAND ao_core_test)

if(AOBUS_BUILD_TUI)
# ao_tui_test: terminal frontend unit tests
add_executable(ao_tui_test
    unit/tui/CoverArtTestSupport.cpp
    unit/tui/TuiRenderTestSupport.cpp
    unit/tui/AnchoredOverlayTest.cpp
    unit/tui/CommandCompletionTest.cpp
    unit/tui/CommandCompletionStateTest.cpp
    unit/tui/CoverArtLoaderTest.cpp
    unit/tui/CoverArtTest.cpp
    unit/tui/EventControllerTest.cpp
    unit/tui/ExecutorTest.cpp
    unit/tui/FrameTimerTest.cpp
    unit/tui/LibraryControllerTest.cpp
    unit/tui/ListNavigationTest.cpp
    unit/tui/LibraryNavigationTest.cpp
    unit/tui/PlaybackStatusFormatterTest.cpp
    unit/tui/QualityIndicatorStyleTest.cpp
    unit/tui/SelectionNavigationTest.cpp
    unit/tui/SignalExitWatcherTest.cpp
    unit/tui/TrackDetailLinesTest.cpp
    unit/tui/TrackListEntryTest.cpp
    unit/tui/TrackPresentationNavigationTest.cpp
    unit/tui/TrackSectionTest.cpp
    unit/tui/OutputDeviceControllerTest.cpp
    unit/tui/PlaybackActionsTest.cpp
    unit/tui/PlaybackPanelTest.cpp
    unit/tui/RenderTest.cpp
    unit/tui/ShellInteractionModelTest.cpp
    unit/tui/SoulButtonTest.cpp
    unit/tui/TextCellTest.cpp
    unit/tui/TrackTableTest.cpp
    unit/tui/TuiHitRegionsTest.cpp
)

target_link_libraries(ao_tui_test PRIVATE
    aobus-tui-lib
    ao_app_uimodel
    ao_app_runtime
    ao_test_support_runtime
    ao_test_options
    Catch2::Catch2WithMain
    FakeIt::FakeIt-catch
)
add_test(NAME ao_tui_test COMMAND ao_tui_test)

# ao_tui_bench: allocation-bound TUI render micro-benchmarks. EXCLUDE_FROM_ALL so
# it is never built by the gate -- it links mimalloc to mirror the shipping
# aobus-tui allocator, which would collide with the ASan/TSan interceptors that
# --asan/--tsan apply to every in-gate target. Build explicitly and run directly:
#   ./ao build --target ao_tui_bench && <builddir>/test/ao_tui_bench
add_executable(ao_tui_bench EXCLUDE_FROM_ALL
    unit/tui/TrackTableBenchmark.cpp
)
target_link_libraries(ao_tui_bench PRIVATE aobus-tui-lib ao_app_uimodel ao_app_runtime ao_test_options Catch2::Catch2WithMain mimalloc)
endif()

if(AOBUS_BUILD_CLI)
# ao_cli_test: command-line frontend tests
add_executable(ao_cli_test
    unit/cli/CliTestSupport.cpp
    unit/cli/CommandErrorTest.cpp
    unit/cli/CliRuntimeTest.cpp
    unit/cli/CliSmokeTest.cpp
    unit/cli/OutputTest.cpp
)

target_link_libraries(ao_cli_test PRIVATE
    aobus-cli-lib
    ao_test_support_audio
    ao_test_support_library
    ao_test_options
    Catch2::Catch2WithMain
)
add_test(NAME ao_cli_test COMMAND ao_cli_test)
endif()

if(AOBUS_BUILD_TOOLS)
# ao_council_test: council tool unit and integration tests
add_executable(ao_council_test
    council/TestSupport.cpp
    unit/council/CouncilSchemaTest.cpp
    unit/council/IntentSerializationTest.cpp
    unit/council/RegistrySerializationTest.cpp
    unit/council/SchedulerTest.cpp
    unit/council/SubstrateTest.cpp
    unit/council/YamlStreamSerializationTest.cpp
    integration/council/CliTest.cpp
    integration/council/RunnerTest.cpp
    integration/council/ProcessRunnerTest.cpp
    integration/council/SubstrateTest.cpp
)
target_include_directories(ao_council_test PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ao_council_test PRIVATE ao_council ao_test_support_common Catch2::Catch2WithMain)
target_compile_definitions(ao_council_test PRIVATE
  AOBUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  AOBUS_COUNCIL_EXE="$<TARGET_FILE:aobus-council>"
)
add_dependencies(ao_council_test aobus-council)
add_test(NAME ao_council_test COMMAND ao_council_test)
endif()

# ao_integration_test: standalone integration tests (full gate only, not the default ./ao test loop)
add_executable(ao_integration_test
    integration/audio/DecoderIntegrationTest.cpp
    integration/audio/GraphIntegrityTest.cpp
    integration/media/file/FileTest.cpp
)

target_link_libraries(ao_integration_test PRIVATE
    ao_app_uimodel
    ao_test_support_library
    ao_test_options
    Catch2::Catch2WithMain
    FakeIt::FakeIt-catch
)
if(LINUX)
  target_sources(ao_integration_test PRIVATE integration/audio/backend/PipeWireProviderTest.cpp)
endif()

if(WIN32)
  target_sources(ao_integration_test PRIVATE integration/audio/backend/WasapiProviderTest.cpp)
endif()

# Not using add_test() here to keep it out of default 'ctest' runs, 
# or use labels/properties if preferred. For now, it's just a manual driver.


if(AOBUS_BUILD_GTK)
# ao_gtk_test: Linux UI tests (require aobus-gtk-lib and GTKMM)
add_executable(ao_gtk_test
    unit/linux-gtk/common/DismissControllerTest.cpp
    unit/linux-gtk/common/MainContextCallbackScopeTest.cpp
    unit/linux-gtk/common/PopoverAttachmentTest.cpp
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
    unit/linux-gtk/image/ResourceImageLoaderTest.cpp
    unit/linux-gtk/app/AppDialogTest.cpp
    unit/linux-gtk/app/ThemeCoordinatorTest.cpp
    unit/linux-gtk/app/ThemePresetTest.cpp
    unit/linux-gtk/app/MainWindowCoordinatorTest.cpp
    unit/linux-gtk/app/MainWindowSessionPresentationTest.cpp
    unit/linux-gtk/app/MenuControllerTest.cpp
    unit/linux-gtk/app/PlaybackShortcutPolicyTest.cpp
    unit/linux-gtk/app/GtkAccelTranslatorTest.cpp
    unit/linux-gtk/app/KeymapApplicatorTest.cpp
    unit/linux-gtk/app/LibraryWindowLifecycleTest.cpp
    unit/linux-gtk/preference/ShortcutEditorWidgetTest.cpp
    unit/linux-gtk/app/MouseNavigationPolicyTest.cpp
    unit/linux-gtk/app/AobusSoulTest.cpp
    unit/linux-gtk/app/GtkStyleRuntimeTest.cpp
    unit/linux-gtk/app/GtkMainContextExecutorTest.cpp
    unit/linux-gtk/app/AppConfigStoreTest.cpp
    unit/linux-gtk/app/MainWindowTest.cpp
    unit/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp
    unit/linux-gtk/app/ShellLayoutStoreTest.cpp
    unit/linux-gtk/app/ShellLayoutControllerTest.cpp
    unit/linux-gtk/app/GtkLayoutStateStoreTest.cpp
    unit/linux-gtk/preference/PreferencesWindowTest.cpp
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
    unit/linux-gtk/status/ActivityStatusWidgetTest.cpp
    unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp
    unit/linux-gtk/portal/LibraryImportExportWorkflowTest.cpp
    unit/linux-gtk/platform/AudioBackendBootstrapTest.cpp
    unit/linux-gtk/platform/MprisArtUrlSessionTest.cpp
    unit/linux-gtk/platform/MprisBridgeTest.cpp
      unit/linux-gtk/layout/document/LayoutModelTest.cpp
      unit/linux-gtk/layout/runtime/ActionRegistryTest.cpp
      unit/linux-gtk/layout/runtime/ActionBinderTest.cpp
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
      unit/linux-gtk/layout/components/TrackFieldGridTextTest.cpp
      unit/linux-gtk/layout/components/PlaybackImageTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorAbsoluteCanvasTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorDescriptorTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorDialogSessionTest.cpp
      unit/linux-gtk/layout/editor/LayoutEditorTemplateTest.cpp
      unit/linux-gtk/layout/components/StatusComponentsTest.cpp
    unit/linux-gtk/playback/AudioPipelinePanelTest.cpp
    unit/linux-gtk/playback/AobusSoulWindowTest.cpp
    unit/linux-gtk/playback/OutputDevicePopoverTest.cpp
    unit/linux-gtk/playback/PlaybackUiComponentsTest.cpp
    unit/linux-gtk/playback/PlaybackDetailsWidgetTest.cpp
    unit/linux-gtk/playback/NowPlayingFieldLabelTest.cpp
    unit/linux-gtk/playback/NowPlayingStatusLabelTest.cpp
    unit/linux-gtk/playback/OutputDeviceListItemsTest.cpp
    unit/linux-gtk/playback/TransportButtonTest.cpp
    unit/linux-gtk/playback/VolumeControlWidgetTest.cpp
    unit/linux-gtk/GtkTestMain.cpp
)

target_link_libraries(ao_gtk_test PRIVATE
    aobus-gtk-lib
    ao_test_support_gtk
    ao_test_support_runtime
    PkgGTKMM
    ao_test_options
    Catch2::Catch2
    FakeIt::FakeIt-catch
)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(ao_gtk_test PRIVATE -Wl,-u,aobus_get_resource)
endif()
add_test(NAME ao_gtk_test COMMAND ao_gtk_test)
endif()

# ao_perf_baseline: standalone performance baseline (not run by default in ./ao check)
add_executable(ao_perf_baseline
    perf/CompletionVocabularyBaselineTest.cpp
    perf/PerformanceBaselineTest.cpp
)

target_link_libraries(ao_perf_baseline PRIVATE
    ao_app_uimodel
    ao_test_support_runtime
    ao_test_options
    Catch2::Catch2WithMain
    FakeIt::FakeIt-catch
)

# Compilation database support for custom linter integration fixture
file(GLOB_RECURSE LINT_FIXTURE_SOURCES "integration/lint/fixture/*.cpp")
add_library(ao_lint_fixture OBJECT EXCLUDE_FROM_ALL ${LINT_FIXTURE_SOURCES})
target_include_directories(ao_lint_fixture PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/integration/lint/fixture")
