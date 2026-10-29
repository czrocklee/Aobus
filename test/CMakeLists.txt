include("${CMAKE_SOURCE_DIR}/cmake/AobusTestSources.cmake")

# ao_test_options: shared test compilation settings
add_library(ao_test_options INTERFACE)
target_include_directories(ao_test_options INTERFACE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app")
target_compile_options(ao_test_options INTERFACE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wno-array-bounds>
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,15>>:-Wno-mismatched-new-delete>
)

set_property(SOURCE
  audio/AudioFixtureSupport.cpp
  cli/CliTestSupport.cpp
  audio/AacDecoderSessionTest.cpp
  audio/AlacDecoderMalformedMp4Test.cpp
  media/mp4/DemuxerTest.cpp
  runtime/library/LibraryCommandsDryRunTest.cpp
  runtime/library/LibraryCommandsTrackCreateCommandTest.cpp
  runtime/library/LibraryExportImportDeltaTest.cpp
  runtime/library/LibraryExportImportTest.cpp
  runtime/resource/ResourceByteReadTest.cpp
  audio/DecoderIntegrationTest.cpp
  audio/GraphIntegrityTest.cpp
  media/file/MediaFileFixtureTest.cpp
  systemmedia/linux/MprisBridgeTest.cpp
  APPEND PROPERTY COMPILE_DEFINITIONS AUDIO_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/data/audio"
)
set_property(SOURCE
  runtime/library/LibraryFormatFixtureTest.cpp
  runtime/library/LibraryYamlModeFixtureTest.cpp
  APPEND PROPERTY COMPILE_DEFINITIONS LIBRARY_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/data/library"
)
set_property(SOURCE
  uimodel/layout/component/LayoutSchemaTest.cpp
  APPEND PROPERTY COMPILE_DEFINITIONS AOBUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
# GCC 16 attributes a reply vector destroyed inside this file's Asio coroutine
# frames to the frame itself after inlining and reports a non-heap free.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
  set_property(SOURCE
    uimodel/library/list/ListOrderAuthoringSessionTest.cpp
    APPEND PROPERTY COMPILE_OPTIONS -Wno-free-nonheap-object)
endif()

if(APPLE)
  # Resolves AOBUS_TEST_COREAUDIO_DEVICE_UID for the Core Audio backend cases
  # and the AppKit scenarios.
  add_library(ao_test_support_coreaudio STATIC
    audio/backend/CoreAudioTestSupport.cpp
  )
  target_include_directories(ao_test_support_coreaudio PUBLIC "${CMAKE_SOURCE_DIR}")
  target_link_libraries(ao_test_support_coreaudio PUBLIC ao_audio PRIVATE ao_test_options)

  add_executable(ao_coreaudio_output_probe helper/CoreAudioOutputProbeMain.cpp)
  target_link_libraries(ao_coreaudio_output_probe PRIVATE ao_test_support_coreaudio)

  # This opt-in GUI harness needs a console session and disposable media. It is
  # built by native checks but deliberately excluded from the unattended suites.
  add_executable(ao_appkit_smoke MACOSX_BUNDLE
    macos-appkit/AppKitSmokeMain.mm
    macos-appkit/AppKitScenarioSupport.mm
    macos-appkit/AppKitScenarioSupport.h
    macos-appkit/AppKitDesktopScenario.mm
    macos-appkit/AppKitDesktopScenarioHelpers.h
    macos-appkit/AppKitDesktopScenarioHelpers.mm
    macos-appkit/AppKitAuthoringScenario.mm
    macos-appkit/AppKitActivityExpirationScenario.mm
    macos-appkit/AppKitArtworkScenario.mm
    macos-appkit/AppKitMediaScenario.mm
    macos-appkit/AppKitPresentationScenario.mm
    macos-appkit/AppKitPlaybackScenario.mm
    macos-appkit/AppKitPlaybackScenario.h
    macos-appkit/AppKitSoulScenario.mm
    media/wav/TestWav.cpp
    media/wav/TestWav.h
  )
  target_link_libraries(ao_appkit_smoke PRIVATE ao_appkit ao_test_support_coreaudio)
  target_include_directories(ao_appkit_smoke PRIVATE "${CMAKE_SOURCE_DIR}")
  set_target_properties(ao_appkit_smoke PROPERTIES
    OBJCXX_STANDARD 26 OBJCXX_STANDARD_REQUIRED ON
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.aobus.desktop.smoke"
    MACOSX_BUNDLE_BUNDLE_NAME "Aobus Native Tests")
endif()

# Owned subprocess coverage uses minimal self-reentering executables so fatal
# startup, abort behavior, and normal observations stay independent of Catch2.
add_library(ao_fatal_probe_support STATIC
    fatal/AudioFatalProbeProtocol.cpp
    fatal/FatalProbeProtocol.cpp
    fatal/LibraryProbeProtocol.cpp
    fatal/RuntimeFatalProbeProtocol.cpp
)
target_include_directories(ao_fatal_probe_support PUBLIC "${CMAKE_SOURCE_DIR}")
if(NOT WIN32)
  target_sources(ao_fatal_probe_support PRIVATE fatal/ProbeProcessPosix.cpp)
endif()
if(WIN32)
  target_sources(ao_fatal_probe_support PRIVATE fatal/ProbeProcessWindows.cpp)
endif()

add_executable(ao_fatal_probe
    fatal/FatalProbeMain.cpp
    fatal/FatalProbeScenario.cpp
)
aobus_enable_windows_utf8_manifest(ao_fatal_probe)
target_include_directories(ao_fatal_probe PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_fatal_probe PRIVATE ao_fatal_probe_support ao_prepared_next_contract ao_utility)
add_test(NAME ao_fatal_probe COMMAND ao_fatal_probe)

add_executable(ao_audio_fatal_probe
    fatal/AudioFatalProbeMain.cpp
    fatal/AudioFatalProbeScenario.cpp
)
aobus_enable_windows_utf8_manifest(ao_audio_fatal_probe)
target_link_libraries(ao_audio_fatal_probe PRIVATE ao_fatal_probe_support ao_test_support_audio)
if(APPLE)
  target_sources(ao_audio_fatal_probe PRIVATE fatal/AudioFatalProbeScenarioMacos.cpp)
  target_link_libraries(ao_audio_fatal_probe PRIVATE ao_test_support_coreaudio)
endif()
add_test(NAME ao_audio_fatal_probe COMMAND ao_audio_fatal_probe)

# Concrete test support is compiled once per layer. Keep these libraries
# dependency-directed: a narrow test must not inherit runtime or frontend
# headers merely to use a temporary directory or a Track fixture.
add_library(ao_test_support_common STATIC
    TestFixtureSupport.cpp
    FilesystemTestSupport.cpp
)
if(WIN32)
  target_sources(ao_test_support_common PRIVATE FilesystemTestSupportWindows.cpp)
  target_link_libraries(ao_test_support_common PRIVATE advapi32)
else()
  target_sources(ao_test_support_common PRIVATE FilesystemTestSupportPosix.cpp)
endif()

add_executable(ao_library_probe
    fatal/LibraryProbeMain.cpp
    fatal/LibraryProbeScenario.cpp
)
aobus_enable_windows_utf8_manifest(ao_library_probe)
target_link_libraries(ao_library_probe PRIVATE ao_fatal_probe_support ao_test_support_library ao_library)
add_test(NAME ao_library_probe COMMAND ao_library_probe)

target_link_libraries(ao_test_support_common
    PUBLIC
      ao_test_options
      ao_utility
      Catch2::Catch2
)

add_library(ao_test_support_library STATIC
    library/LibraryBinaryTestSupport.cpp
    library/LibraryStoreTestSupport.cpp
    library/MusicLibraryTestSupport.cpp
    library/TrackBuilderTestSupport.cpp
    library/TrackStoreTestSupport.cpp
    library/TrackTestSupport.cpp
    library/TrackViewTestSupport.cpp
    library/WritableLibraryTestSupport.cpp
    lmdb/LmdbProbeScratch.cpp
)
target_link_libraries(ao_test_support_library
    PUBLIC
      ao_test_support_common
      ao_library
      Catch2::Catch2
)

add_library(ao_test_support_audio STATIC
    audio/AudioFixtureSupport.cpp
    audio/BackendTestSupport.cpp
    audio/DecoderTestSupport.cpp
    audio/FlacFrameTestSupport.cpp
    # Keep Engine-failure futures/assertions out of the shared decoder-fixture
    # object used by the Catch-independent audio fatal probe.
    audio/EngineFailureTestSupport.cpp
    audio/EngineTestSupport.cpp
    audio/FakeCapturingBackend.cpp
    audio/ScriptedDecoderSession.cpp
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
    runtime/AppRuntimeTestSupport.cpp
    runtime/AsyncTestSupport.cpp
    runtime/CueYamlTestSupport.cpp
    runtime/ExecutorTestSupport.cpp
    runtime/PlaybackSuccessionBaseTestSupport.cpp
    runtime/PlaybackSuccessionSeekTestSupport.cpp
    runtime/PlaybackSuccessionTransportTestSupport.cpp
    runtime/PlaybackTestSupport.cpp
    runtime/PlaybackTransportTestSupport.cpp
    runtime/PlaybackUiTestSupport.cpp
    runtime/RuntimeLibraryTestSupport.cpp
    runtime/ViewServiceTestSupport.cpp
    runtime/WorkspaceTestSupport.cpp
    runtime/completion/CompletionTestSupport.cpp
    runtime/library/ScanApplyTestSupport.cpp
    runtime/projection/TrackListProjectionTestSupport.cpp
    runtime/source/ListOrderSourceTestSupport.cpp
    runtime/source/SmartListEvaluatorTestSupport.cpp
    runtime/source/TrackSourceTestSupport.cpp
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
if(WIN32)
  # timeBeginPeriod lets the test executors' 1 ms polls wait about 1 ms.
  target_link_libraries(ao_test_support_runtime PRIVATE winmm)
endif()

add_library(ao_test_support_i18n STATIC
    MessageCatalogTestSupport.cpp
)
target_link_libraries(ao_test_support_i18n
    PUBLIC
      ao_test_options
      ao_app_i18n
)

if(AOBUS_BUILD_GTK)
  pkg_check_modules(XTEST REQUIRED IMPORTED_TARGET xtst x11 xi)

  add_library(ao_test_support_gtk_session_bus STATIC
      linux-gtk/GtkSessionBusTestSupport.cpp
  )
  target_link_libraries(ao_test_support_gtk_session_bus PUBLIC ao_test_options)

  add_executable(ao_gapplication_probe
      linux-gtk/platform/GApplicationReplacementProbeMain.cpp
  )
  target_link_libraries(ao_gapplication_probe
      PRIVATE
        ao_fatal_probe_support
        ao_test_support_gtk_session_bus
        PkgGTKMM
  )

  add_library(ao_test_support_gtk STATIC
      linux-gtk/GtkApplicationTestSupport.cpp
      linux-gtk/GtkLayoutTestSupport.cpp
      linux-gtk/GtkNativeInputTestSupport.cpp
      linux-gtk/GtkRuntimeTestSupport.cpp
      linux-gtk/GtkWidgetTestSupport.cpp
      linux-gtk/image/ImageTestSupport.cpp
      linux-gtk/layout/LayoutTestSupport.cpp
      linux-gtk/layout/components/ContainerTestHelpers.cpp
      linux-gtk/layout/state/FakeLayoutComponentStateStore.cpp
      linux-gtk/track/TrackSelectionControllerTestSupport.cpp
  )
  target_link_libraries(ao_test_support_gtk
      PUBLIC
        ao_test_options
        ao_test_support_gtk_session_bus
        ao_test_support_i18n
        aobus-gtk-lib
        PkgGTKMM
      PRIVATE
        ao_test_support_runtime
        ao_test_support_library
        ao_test_support_common
        PkgConfig::XTEST
        Catch2::Catch2
  )
endif()

add_library(ao_test_support_performance STATIC
    perf/report/PerformanceReport.cpp
)
target_link_libraries(ao_test_support_performance
    PUBLIC
      ao_test_options
      ao_utility
    PRIVATE
      ICU::uc
)

# ao_core_test: shared and host-native unit + integration tests
add_executable(ao_core_test
    CoreTestMain.cpp
    perf/report/PerformanceReportTest.cpp
    fatal/RuntimeFatalProbeScenario.cpp

    # async tests
    async/RequestCoalescerTest.cpp
    async/SignalTest.cpp

    # utility tests
    utility/AtomicFileTest.cpp
    utility/FileAllocationTest.cpp
    utility/RaiiTest.cpp
    utility/Base64Test.cpp
    utility/Sha256Test.cpp
    utility/Xxh3Test.cpp
    utility/MappedFileTest.cpp
    utility/PathTest.cpp
    utility/PlatformDirectoriesTest.cpp
    utility/ByteViewTest.cpp
    utility/CallbackStackScopeTest.cpp
    utility/StringTest.cpp
    runtime/projection/StringArenaTest.cpp
    utility/UnicodeTextTest.cpp
    utility/StrongTypeTest.cpp
    utility/UuidTest.cpp
    utility/YamlReflectTest.cpp
    utility/RymlAdapterTest.cpp
    utility/ScopedRegistrationTest.cpp
    utility/YamlSerializationTest.cpp
    utility/TestFixtureSupportTest.cpp
    utility/FromCharsTest.cpp

    # compatibility tests
    compat/MoveOnlyFunctionTest.cpp
    compat/EnumerateTest.cpp
    compat/AtomicSharedPtrTest.cpp

    # core tests
    core/AudioCodecTest.cpp
    core/ErrorTest.cpp
    core/FatalTest.cpp
    core/FatalProbeTest.cpp

    # localization foundation tests
    i18n/AppKitMessageCatalogTest.cpp
    i18n/CatalogPatternTest.cpp
    i18n/IcuTextOrderingTest.cpp
    i18n/IcuCompletionAliasesTest.cpp
    i18n/MessageCatalogTest.cpp

    # query tests
    query/CompletionBaselineTest.cpp
    query/CompletionLogicalOperatorTest.cpp
    query/CompletionOperatorTest.cpp
    query/CompletionValueTest.cpp
    query/CompletionVariableTest.cpp
    query/CompletionTokenizerTest.cpp
    query/ParserTest.cpp
    query/PredicateTest.cpp
    query/ExecutionPlanAccessProfileTest.cpp
    query/ExecutionPlanCompileTest.cpp
    query/ExecutionPlanContractTest.cpp
    query/ExecutionPlanDictionaryTest.cpp
    query/ExecutionPlanFieldCatalogTest.cpp
    query/ExecutionPlanTestSupport.cpp
    query/ExecutionPlanMembershipTest.cpp
    query/ExecutionPlanStringExpressionTest.cpp
    query/ExecutionPlanTagBloomTest.cpp
    query/ExecutionPlanUnitLiteralTest.cpp
    query/FormatExpressionTest.cpp
    query/RecordingDatePredicateTest.cpp
    query/PredicateOperandContextTest.cpp
    query/CreditPredicateTest.cpp
    query/PlanEvaluatorTestSupport.cpp
    query/PlanEvaluatorCollectionTest.cpp
    query/PlanEvaluatorComparisonTest.cpp
    query/PlanEvaluatorDictionaryTest.cpp
    query/PlanEvaluatorExecutionTest.cpp
    query/PlanEvaluatorLogicalTest.cpp
    query/PlanEvaluatorMediaPropertyTest.cpp
    query/PlanEvaluatorStringTest.cpp
    query/PlanEvaluatorTagTest.cpp
    query/ExpressionTest.cpp
    query/SerializerTest.cpp
    query/QueryErrorTest.cpp

    # media file tests
    media/file/MediaFileFixtureTest.cpp
    media/file/TestFile.cpp
    media/file/ApeTagTest.cpp
    media/file/MpegFrameTest.cpp
    media/file/MpegFileTest.cpp
    media/file/MpegFileTrailingTagTest.cpp
    media/file/Mp4FileTest.cpp
    media/file/id3v2/ReaderTest.cpp
    media/file/id3v2/TestId3v2.cpp
    media/file/id3v2/CreditReaderTest.cpp
    media/file/ContentCreditTest.cpp
    media/file/ContentTextTest.cpp
    media/file/VorbisCreditTest.cpp
    media/file/FlacFileTest.cpp
    media/file/WavFileTest.cpp
    media/file/WavCreditContentTest.cpp
    media/file/OpusFileTest.cpp
    media/file/FileTest.cpp
    media/file/DecoderTest.cpp

    # media tests
    media/CueSheetTest.cpp
    media/ExactExtentTest.cpp
    media/VorbisCommentTest.cpp
    media/ogg/TestOgg.cpp
    media/ogg/DemuxerTest.cpp
    media/opus/HeaderTest.cpp
    media/opus/TimelineTest.cpp
    media/mp4/TestAtoms.cpp
    media/mp4/AtomTest.cpp
    media/mp4/DemuxerTest.cpp
    media/mp4/DemuxerAlacLayoutTest.cpp
    media/mp4/SampleDescriptionTest.cpp
    media/wav/TestWav.cpp
    media/wav/RiffTest.cpp
    media/wav/RiffChannelMaskTest.cpp

    # library tests
    library/RecordingDateTest.cpp
    library/CreditsTest.cpp
    library/TrackLayoutTest.cpp
    library/TrackPerformanceTest.cpp
    library/TrackPerformanceValidationTest.cpp
    library/TrackViewTest.cpp
    library/TrackViewTagTest.cpp
    library/TrackViewCustomMetadataTest.cpp
    library/TrackViewValidationTest.cpp
    library/FileManifestViewTest.cpp
    library/FileTimeConversionTest.cpp
    library/FileManifestStoreTest.cpp
    library/LibraryUriTest.cpp
    library/FileManifestBuilderTest.cpp
    library/TrackBuilderTest.cpp
    library/TrackBuilderCoverArtTest.cpp
    library/TrackBuilderTagTest.cpp
    library/TrackBuilderSerializationTest.cpp
    library/TrackBuilderOverflowTest.cpp
    library/LibraryErrorTest.cpp
    library/LibraryProbeTest.cpp
    library/MetadataStoreTest.cpp
    library/ListLayoutTest.cpp
    library/ListViewTest.cpp
    library/DictionaryStoreTest.cpp
    library/AudioIdentityTest.cpp
    library/ListStoreTest.cpp
    library/ResourceLayoutTest.cpp
    library/ResourceStoreTest.cpp
    library/WriteTransactionTest.cpp
    library/IdSpaceTest.cpp
    library/TrackWriterTest.cpp
    library/CarrierTrackTest.cpp
    library/CarrierAdmissionRegressionTest.cpp
    library/ListWriterTest.cpp

    library/TrackStoreTest.cpp
    library/TrackWriteTest.cpp
    library/TrackStoreRawLayoutTest.cpp
    library/TrackStoreIntegrityTest.cpp
    library/ListBuilderTest.cpp
    library/MusicLibraryTest.cpp

    # lmdb tests
    lmdb/LmdbTestSupport.cpp
    lmdb/LmdbTestSupportTest.cpp
    lmdb/ReadOnlyDatabaseTest.cpp
    lmdb/ReadOnlyDatabaseLifetimeTest.cpp
    lmdb/EnvironmentTest.cpp
    lmdb/EnvironmentFileGrowthTest.cpp
    lmdb/MapCapacityPolicyTest.cpp
    lmdb/TransactionTest.cpp
    lmdb/DatabaseTest.cpp
    lmdb/DatabaseReaderTest.cpp
    lmdb/DatabaseMaxKeyTest.cpp
    lmdb/DatabaseWriterTest.cpp
    lmdb/DatabaseWriterLifetimeTest.cpp
    lmdb/DatabaseByteKeyTest.cpp
    lmdb/ResultErrorTest.cpp

    # audio unit tests
    audio/DecoderIntegrationTest.cpp
    audio/GraphIntegrityTest.cpp
    audio/DecoderFactoryTest.cpp
    audio/PlatformBackendProvidersTest.cpp
    audio/DecoderOutputTest.cpp
    audio/OpenedModeValidationTest.cpp
    audio/detail/DecoderOutputAdapterTest.cpp
    audio/NullBackendTest.cpp
    audio/detail/MappedFileCursorTest.cpp
    audio/detail/Mp4PacketSourceTest.cpp
    audio/detail/TimeConversionTest.cpp
    audio/AudioTimeTest.cpp
    audio/detail/StreamingBufferPolicyTest.cpp
    audio/detail/TrackSessionTest.cpp
    audio/DecoderErrorTest.cpp
    audio/AacDecoderSessionTest.cpp
    audio/AacDecoderMalformedMp4Test.cpp
    audio/AacDecoderChannelLayoutTest.cpp
    audio/FlacDecoderSessionTest.cpp
    audio/FlacDecoderChannelLayoutTest.cpp
    audio/FlacCommentCompatibilityTest.cpp
    audio/FlacMetadataInitializationTest.cpp
    audio/FlacWavMaskCompatibilityTest.cpp
    audio/FlacDecoderMalformedTest.cpp
    audio/FlacDecoderFormatStabilityTest.cpp
    audio/AlacDecoderSessionTest.cpp
    audio/AlacDecoderMalformedMp4Test.cpp
    audio/AlacFrameTestSupport.cpp
    audio/AlacDecoderChannelLayoutTest.cpp
    audio/AlacChannelMetadataTest.cpp
    audio/Mp3DecoderSessionTest.cpp
    audio/Mp3DecoderChannelLayoutTest.cpp
    audio/Mp3DecoderTrailingTagTest.cpp
    audio/OpusDecoderSessionTest.cpp
    audio/OpusDecoderChannelLayoutTest.cpp
    audio/OpusDecoderMalformedTest.cpp
    audio/WavDecoderSessionTest.cpp
    audio/WavDecoderChannelLayoutTest.cpp
    audio/SegmentDecoderSessionTest.cpp
    audio/Mp3DecoderMalformedTest.cpp
    audio/QualityAnalyzerTest.cpp
    audio/ChannelQualityTest.cpp
    audio/ChannelLayoutEvidenceTest.cpp
    audio/FormatTest.cpp
    audio/StreamingSourceTest.cpp
    audio/StreamingPrerollTest.cpp
    audio/StreamingRenderAlignmentTest.cpp
    audio/PcmFrameRingTest.cpp
    audio/backend/detail/AudioBackendRenderBufferTest.cpp
    audio/backend/detail/AudioBackendRenderProgressTest.cpp
    audio/backend/detail/AudioBackendDrainTailTest.cpp
    audio/backend/detail/BackendDeviceRegistryTest.cpp
    audio/backend/detail/BackendGraphRegistryTest.cpp
    audio/backend/detail/CallbackFenceTest.cpp
    audio/PcmConversionTest.cpp
    audio/PlayerTest.cpp
    audio/PlayerGraphObservationTest.cpp
    audio/EngineDrainTest.cpp
    audio/EngineErrorTest.cpp
    audio/EngineControlErrorTest.cpp
    audio/EngineFatalProbeTest.cpp
    audio/EngineGaplessTest.cpp
    audio/EngineChannelLayoutTest.cpp
    audio/EngineWavChannelLayoutTest.cpp
    audio/EngineFlacChannelLayoutTest.cpp
    audio/EngineFlacTransitionTest.cpp
    audio/EngineAlacChannelLayoutTest.cpp
    audio/EngineStereoIdentityTest.cpp
    audio/EngineStereoReuseTest.cpp
    audio/EngineEndpointPrecisionTest.cpp
    audio/EngineRtSignalRingTest.cpp
    audio/EngineSeekErrorTest.cpp
    audio/EngineTest.cpp
    audio/EngineBackendFailureTest.cpp
    audio/EngineBackendSwapTest.cpp
    audio/EngineCallbackTest.cpp
    audio/EngineConcurrencyTest.cpp
    audio/EnginePropertyTest.cpp
    audio/EngineTransportTest.cpp

    # ui model tests (no platform deps)
    uimodel/field/TrackFieldFormatterTest.cpp
    uimodel/input/KeyChordTest.cpp
    uimodel/input/KeymapModelTest.cpp
    uimodel/input/KeyRepeatGuardTest.cpp
    uimodel/input/KeymapStoreTest.cpp
    uimodel/preference/NewerConfigNoticeTest.cpp
    uimodel/preference/PreferencesEditorModelTest.cpp
    uimodel/preference/ThemePresetTest.cpp
    uimodel/layout/component/LayoutSchemaTest.cpp
    uimodel/layout/shell/LayoutSessionTest.cpp
    uimodel/layout/component/AbsoluteCanvasGeometryTest.cpp
    uimodel/layout/component/LayoutComponentStateTest.cpp
    uimodel/layout/component/LayoutStatePromoterTest.cpp
    uimodel/layout/document/LayoutModelTest.cpp
    uimodel/layout/document/LayoutPlacementTest.cpp
    uimodel/layout/document/LayoutPreparationTest.cpp
    uimodel/layout/document/LayoutValidationTest.cpp
    uimodel/layout/document/LayoutTemplateExpansionTest.cpp
    uimodel/layout/shell/ShellGenerationSequenceTest.cpp
    uimodel/presentation/CoverArtPlaceholderTest.cpp
    uimodel/presentation/PresentationTextFeaturesTest.cpp
    uimodel/library/detail/TrackCustomMetadataTest.cpp
    uimodel/library/detail/TrackCreditsTest.cpp
    uimodel/library/detail/TrackFieldGridVisibilityTest.cpp
    uimodel/library/detail/TrackFieldGridSchemaTest.cpp
    uimodel/library/list/ListActionsTest.cpp
    uimodel/library/list/ListAuthoringTest.cpp
    uimodel/library/list/ListOrderAuthoringSessionTest.cpp
    uimodel/library/list/ListOrderCapabilitiesTest.cpp
    uimodel/library/list/ListTreeProjectionTest.cpp
    uimodel/library/list/SmartListEditingTest.cpp
    uimodel/library/list/SmartListPresentationTest.cpp
    uimodel/library/presentation/CustomPresentationEditorModelTest.cpp
    uimodel/library/presentation/LibraryTransferPresentationTest.cpp
    uimodel/library/presentation/ListPresentationPreferenceYamlSchemaTest.cpp
    uimodel/library/presentation/ListPresentationsTest.cpp
    uimodel/library/presentation/TrackColumnLayoutYamlSchemaTest.cpp
    uimodel/library/presentation/TrackColumnLayoutMergingTest.cpp
    uimodel/library/presentation/TrackColumnLayoutsTest.cpp
    uimodel/library/presentation/TrackColumnWidthSolverTest.cpp
    uimodel/library/presentation/TrackColumnDefaultsTest.cpp
    uimodel/library/presentation/TrackPresentationCatalogTest.cpp
    uimodel/library/presentation/TrackPresentationPickerViewModelTest.cpp
    uimodel/library/presentation/ListPresentationRecommendationTest.cpp
    uimodel/library/property/TagEditTest.cpp
    uimodel/library/property/TrackPropertiesFormSpecTest.cpp
    uimodel/library/property/TrackPropertiesCreditsTest.cpp
    uimodel/library/property/TrackPropertiesFormModelTest.cpp
    uimodel/library/property/TrackPropertiesFormBaselineTest.cpp
    uimodel/library/task/LibraryScanOutcomeTest.cpp
    uimodel/library/task/LibraryScanWorkflowTest.cpp
    uimodel/library/track/ListMembershipAuthoringSessionTest.cpp
    uimodel/library/track/TrackAuthoringSessionTest.cpp
    uimodel/library/track/TrackAuthoringSnapshotSessionTest.cpp
    uimodel/library/track/TrackAuthoringTest.cpp
    uimodel/library/track/RecordingDateEditingTest.cpp
    uimodel/library/track/TrackAuthoringTestSupport.cpp
    uimodel/library/track/IndexedTrackRowCacheTest.cpp
    uimodel/library/track/TrackDisplayIndexTest.cpp
    uimodel/library/track/TrackFilterCompleterTest.cpp
    uimodel/library/track/TrackFilterResolutionTest.cpp
    uimodel/library/track/TrackFilterViewModelTest.cpp
    uimodel/library/track/TrackPageRouteTest.cpp
    uimodel/library/track/TrackSelectionSummaryTest.cpp
    uimodel/playback/now-playing/NowPlayingViewModelTest.cpp
    uimodel/playback/command/PlaybackCommandTest.cpp
    uimodel/playback/command/PlaybackActionsTest.cpp
    uimodel/playback/output/OutputSelectionTest.cpp
    uimodel/playback/output/OutputDeviceIntentTest.cpp
    uimodel/playback/output/OutputDeviceViewModelTest.cpp
    uimodel/playback/output/VolumeViewModelTest.cpp
    uimodel/playback/quality/AudioQualityFormatterTest.cpp
    uimodel/playback/quality/ChannelLayoutPresentationTest.cpp
    uimodel/playback/quality/DecodedQualityPresentationTest.cpp
    uimodel/playback/seek/PlaybackPositionInterpolatorTest.cpp
    uimodel/playback/seek/PlaybackPositionViewModelTest.cpp
    uimodel/playback/seek/PlaybackTimeFormatterTest.cpp
    uimodel/playback/seek/SeekInteractionTest.cpp
    uimodel/playback/soul/AobusSoulViewModelTest.cpp
    uimodel/playback/transport/TransportViewModelTest.cpp
    uimodel/status/activity/ActivityStatusFeedProjectionTestSupport.cpp
    uimodel/status/activity/ActivityStatusFeedProjectionCompactTest.cpp
    uimodel/status/activity/ActivityStatusFeedProjectionDetailTest.cpp
    uimodel/status/activity/ActivityStatusFeedProjectionNotificationTest.cpp
    uimodel/status/activity/ActivityStatusViewModelTest.cpp

    # app unit tests (no platform deps)
    runtime/AsyncParallelTest.cpp
    runtime/AsyncRuntimeTest.cpp
    runtime/ExecutorTestSupportTest.cpp
    runtime/library/LibraryPathsTest.cpp
    runtime/library/LibrarySnapshotTest.cpp
    runtime/library/LibrarySnapshotCreditsTest.cpp
    runtime/library/LibrarySnapshotCustomMetadataTest.cpp
    runtime/library/MediaTrackTest.cpp
    runtime/completion/CompletionTextTest.cpp
    runtime/completion/CompletionVocabularyTest.cpp
    runtime/completion/CompletionServiceTest.cpp
    runtime/completion/CreditCompletionTest.cpp
    runtime/LibraryRestoreStateIdentityTest.cpp
    runtime/LifetimeScopeTest.cpp
    runtime/LogTest.cpp
    runtime/LoopExecutorTest.cpp
    runtime/source/SmartListEvaluatorBatchTest.cpp
    runtime/source/SmartListEvaluatorExpressionTest.cpp
    runtime/source/SmartListEvaluatorIncrementalTest.cpp
    runtime/source/SmartListEvaluatorLoadModeTest.cpp
    runtime/source/SmartListEvaluatorTest.cpp
    runtime/source/SourcePipelineOperationTest.cpp
    runtime/source/IndexedTrackSequenceTest.cpp
    runtime/source/SourcePipelineOracleTest.cpp
    runtime/source/TrackSourceDeltaTest.cpp
    runtime/source/TrackSourceDeltaBuilderTest.cpp
    runtime/source/TrackSourceLifecycleTest.cpp
    runtime/TrackPresentationTest.cpp
    runtime/TrackEditScriptTest.cpp
    runtime/library/LibraryCommandsTagCommandTest.cpp
    runtime/library/LibraryCommandsTrackPropertiesTest.cpp
    runtime/library/LibraryCommandsTrackCreateCommandTest.cpp
    runtime/library/LibraryCommandsTrackDeleteCommandTest.cpp
    runtime/library/LibraryCommandsListMembershipTest.cpp
    runtime/library/LibraryCommandsListOrderTest.cpp
    runtime/TrackFieldTest.cpp
    runtime/TrackRecordingDateFieldTest.cpp
    runtime/library/LibraryCommandsDryRunTest.cpp
    runtime/library/LibraryChangesTest.cpp
    runtime/projection/TrackListProjectionGroupingTest.cpp
    runtime/projection/TrackListProjectionDeltaContractTest.cpp
    runtime/projection/TrackListProjectionIncrementalTest.cpp
    runtime/projection/TrackListProjectionLifecycleTest.cpp
    runtime/projection/TrackListProjectionMutationTest.cpp
    runtime/projection/TrackListProjectionSortingTest.cpp
    runtime/projection/TrackRecordingDateSortTest.cpp
    runtime/projection/TrackListProjectionScaleTest.cpp
    runtime/playback/PlaybackCursorModelTest.cpp
    runtime/playback/PlaybackRestartDeadlineTest.cpp
    runtime/playback/PreparedNextRegistryTest.cpp
    runtime/playback/ProjectionAnchorTest.cpp
    runtime/playback/ShuffleHistoryTest.cpp
    runtime/PlaybackServiceTest.cpp
    runtime/PlaybackGuardedSeekTest.cpp
    runtime/PlaybackEndSeekTest.cpp
    runtime/CarrierPlaybackTest.cpp
    runtime/CarrierSuccessionTest.cpp
    runtime/CuePlaybackSessionTest.cpp
    runtime/CueYamlTest.cpp
    runtime/CueYamlBaselineTest.cpp
    runtime/CueYamlPreparationTest.cpp
    runtime/PlaybackPositionQueryTest.cpp
    runtime/PlaybackMetadataTest.cpp
    runtime/PlaybackTransportControlTest.cpp
    runtime/PlaybackTransportOutputTest.cpp
    runtime/PlaybackTransportSelectionTest.cpp
    runtime/PlaybackTransportTest.cpp
    runtime/PlaybackTransportTokenTest.cpp
    runtime/PlaybackSuccessionLaunchTest.cpp
    runtime/PlaybackSuccessionProjectionTest.cpp
    runtime/PlaybackSuccessionAdvanceTest.cpp
    runtime/PlaybackSuccessionFailureTest.cpp
    runtime/PlaybackSessionTest.cpp
    runtime/PlaybackSessionVolumeTest.cpp
    runtime/source/AllTracksSourceTest.cpp
    runtime/source/TrackSourcePublicationTest.cpp
    runtime/source/TrackSourceCacheTest.cpp
    runtime/source/TrackSourceCachePublicationLifetimeTest.cpp
    runtime/source/VirtualListIdsTest.cpp
    runtime/projection/TrackDetailProjectionTest.cpp
    runtime/projection/TrackDetailPerformanceTest.cpp
    runtime/library/AudioIdentityIndexerTest.cpp
    runtime/library/LibraryScanTest.cpp
    runtime/library/ScanPlanTest.cpp
    runtime/library/ScanApplyDatabaseAdmissionTest.cpp
    runtime/library/ScanApplyFilesystemRevalidationTest.cpp
    runtime/library/ScanApplyPathProvenanceTest.cpp
    runtime/library/ScanApplyOperationTest.cpp
    runtime/library/ScanCuePreparationTest.cpp
    runtime/library/ScanWorkMetadataTest.cpp
    runtime/library/ScanPerformanceMetadataTest.cpp
    runtime/library/TrackBuilderSnapshotTest.cpp
    runtime/library/LibraryJobsTest.cpp
    runtime/library/RuntimeFatalProbeTest.cpp
    runtime/library/LibraryCommandsTest.cpp
    runtime/library/LibraryAuthoringTest.cpp
    runtime/library/TrackAuthoringSnapshotTest.cpp
    runtime/library/TrackTargetAdmissionTest.cpp
    runtime/library/TrackCustomMetadataAdmissionTest.cpp
    runtime/library/TrackPerformanceAuthoringTest.cpp
    runtime/completion/MetadataValueCompleterTest.cpp
    runtime/NotificationServiceTest.cpp
    runtime/NotificationServiceExpiryTest.cpp
    runtime/WritableTagListTest.cpp
    runtime/completion/QueryExpressionCompleterTest.cpp
    runtime/ViewServiceLifecycleTest.cpp
    runtime/ViewServiceListFilterTest.cpp
    runtime/ViewServicePresentationTest.cpp
    runtime/ViewServiceSelectionTest.cpp
    runtime/ConfigStoreTest.cpp
    runtime/source/ListOrderSourceObserverTest.cpp
    runtime/source/ListOrderSourceTest.cpp
    runtime/NavigationHistoryTest.cpp
    runtime/WorkspaceHistoryTest.cpp
    runtime/WorkspaceNavigationTest.cpp
    runtime/WorkspacePresentationTest.cpp
    runtime/WorkspaceSessionYamlSchemaTest.cpp
    runtime/WorkspaceSessionTest.cpp
    runtime/AppRuntimeTest.cpp
    runtime/AppStateTest.cpp
    runtime/HeadlessShellTest.cpp
    runtime/library/LibraryExportImportCoverArtTest.cpp
    runtime/library/LibraryExportImportDeltaTest.cpp
    runtime/library/LibraryExportImportErrorTest.cpp
    runtime/library/LibraryExportImportListTest.cpp
    runtime/library/LibraryFormatFixtureTest.cpp
    runtime/library/CueScanTest.cpp
    runtime/library/CueSheetAccessTest.cpp
    runtime/library/CueCarrierFactsTest.cpp
    runtime/library/CueRelinkPlanTest.cpp
    runtime/library/LibraryYamlSchemaTest.cpp
    runtime/library/LibraryYamlExporterTest.cpp
    runtime/library/LibraryYamlImporterTest.cpp
    runtime/library/LibraryYamlPerformanceMetadataTest.cpp
    runtime/library/LibraryYamlCreditBoundaryTest.cpp
    runtime/library/LibraryYamlModeFixtureTest.cpp
    runtime/library/LibraryExportImportTest.cpp
    runtime/resource/ResourceByteDiskCacheTest.cpp
    runtime/resource/ResourceByteMemoryCacheTest.cpp
    runtime/resource/ResourceByteReadTest.cpp
    runtime/resource/ResourceByteReaderTest.cpp
    desktop/DetachedProcessLauncherTest.cpp
    desktop/DetachedProcessLauncherWindowsTest.cpp
    desktop/LibraryStartupPlannerTest.cpp
    desktop/LibrarySuccessorProtocolTest.cpp
    desktop/LibrarySwitchTest.cpp

    # Every Windows shell source that carries no WinRT dependency is gated on
    # every host rather than only on a native Windows run: settings
    # compatibility, output-preference resolution, the root-commit transaction,
    # restart sequencing, the component catalog and dialect, placement and
    # style resolution, shell-document validation, theme tokens, and the
    # keyboard-accelerator plan are all decided in plain C++, and are the rules
    # a Linux-only change is most likely to break unnoticed. The catalog earns
    # its place here twice over, since it is where Windows agrees with the
    # shared component vocabulary.
    #
    # The rule is "no WinRT, so every host"; anything left in the WIN32 block
    # below is there because it needs native types, not because it is Windows
    # policy.
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/DesktopOutputSelection.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/DesktopSettingsYamlSchema.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/DestructiveLibraryRestart.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/DispatcherQueueAdmission.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/SelectedRootCommit.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/SettingsPersistence.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/app/StartupWarnings.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ElementKind.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/LayoutSchema.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/LayoutDialect.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/PlacementPlan.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ShellCommands.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ShellDocument.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ShellState.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/StyleLookup.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ThemeSurface.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/theme/Theme.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/input/KeyChordAccelerator.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/input/KeymapAcceleratorPlan.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/library/LibraryTransferAdapter.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/track/QuickFilterCompletionAdapter.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/track/TrackRevealAdapter.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/track/TrackPropertiesAdapter.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/list/ListAuthoringAdapter.cpp
    winui/DesktopSettingsYamlSchemaTest.cpp
    winui/ThemeTest.cpp
    winui/app/CallbackAdmissionGateTest.cpp
    winui/app/DesktopOutputSelectionTest.cpp
    winui/app/DestructiveLibraryRestartTest.cpp
    winui/app/DispatcherQueueAdmissionTest.cpp
    winui/app/SelectedRootCommitTest.cpp
    winui/app/SettingsPersistenceTest.cpp
    winui/app/StartupWarningsTest.cpp
    winui/input/KeyChordAcceleratorTest.cpp
    winui/input/KeymapAcceleratorPlanTest.cpp
    winui/library/LibraryTransferAdapterTest.cpp
    winui/layout/ElementKindTest.cpp
    winui/layout/LayoutSchemaTest.cpp
    winui/layout/LayoutDialectTest.cpp
    winui/layout/PlacementPlanTest.cpp
    winui/layout/ShellCommandsTest.cpp
    winui/layout/ShellCommandWiringTest.cpp
    winui/layout/ShellDocumentTest.cpp
    winui/layout/ShellStateTest.cpp
    winui/layout/StyleLookupTest.cpp
    winui/layout/ThemeSurfaceTest.cpp
    winui/track/QuickFilterCompletionAdapterTest.cpp
    winui/track/TrackRevealAdapterTest.cpp
    winui/track/TrackPropertiesAdapterTest.cpp
    winui/list/ListAuthoringAdapterTest.cpp
)
aobus_enable_windows_utf8_manifest(ao_core_test)
target_include_directories(ao_core_test PRIVATE
    "${CMAKE_SOURCE_DIR}/app/windows-winui/include")

# The shipped Windows presets are read by the cross-host document tests, which
# is the point of gating them everywhere: a change made on Linux to shared
# layout validation must fail here rather than on the next Windows run.
target_compile_definitions(ao_core_test PRIVATE
    AOBUS_WINDOWS_LAYOUT_DIR="${CMAKE_SOURCE_DIR}/app/windows-winui/layout"
    AOBUS_WINDOWS_SHELL_BUILDER_SOURCE="${CMAKE_SOURCE_DIR}/app/windows-winui/layout/ShellBuilder.cpp"
    AOBUS_WINDOWS_WINUI_CMAKE="${CMAKE_SOURCE_DIR}/app/windows-winui/CMakeLists.txt"
    AOBUS_WINDOWS_WINUI_DIR="${CMAKE_SOURCE_DIR}/app/windows-winui")

target_link_libraries(ao_core_test PRIVATE
    PkgFDKAAC
    ao_fatal_probe_support
    ao_desktop_launch
    ao_app_uimodel
    ao_app_i18n
    ao_app_i18n_completion
    ao_app_i18n_ordering
    ao_catalog_pattern
    ao_unicode_text
    ao_test_support_i18n
    ao_test_support_performance
    ao_test_support_runtime
    ao_test_options
    Catch2::Catch2
    FakeIt::FakeIt-catch
)
if(LINUX)
  target_link_libraries(ao_core_test PRIVATE PkgALSA PkgPipeWire)
endif()
if(LINUX)
  target_sources(ao_core_test PRIVATE audio/backend/AlsaMixerTestSupport.cpp)
endif()
aobus_target_test_sources(ao_core_test "${LINUX}" "non-Linux host"
  audio/backend/PipeWireMonitorTest.cpp
  audio/backend/PipeWireBackendTest.cpp
  audio/backend/AlsaEnginePropertyTest.cpp
  audio/backend/AlsaExclusiveBackendTest.cpp
  audio/backend/AlsaBackendReuseTest.cpp
  audio/backend/AlsaProviderTest.cpp
  audio/backend/FlacAlsaPipeWireRouteTest.cpp
  audio/backend/OpusAlsaChannelLayoutTest.cpp
  audio/backend/AacAlsaChannelLayoutTest.cpp
  audio/backend/detail/PipeWireMonitorParsingTest.cpp
  audio/backend/detail/PipeWireFormatParsingTest.cpp
  audio/backend/detail/AlsaGraphRegistryTest.cpp
  audio/backend/detail/AlsaChannelMapTest.cpp
  audio/backend/detail/AlsaLegacyChannelMapTest.cpp
  audio/backend/detail/AlsaPrewarmCacheTest.cpp
  audio/backend/detail/AlsaMixerSessionTest.cpp
  audio/backend/detail/AlsaModeSelectorTest.cpp
  audio/backend/detail/AlsaPcmErrorTest.cpp
  audio/backend/detail/AlsaPcmFormatTest.cpp
  audio/backend/detail/AlsaPcmVolumeTest.cpp
  runtime/PlaybackSessionAlsaMuteTest.cpp
)
aobus_target_test_sources(ao_core_test "${APPLE}" "non-macOS host"
  audio/backend/CoreAudioBackendFatalProbeTest.cpp
  audio/backend/CoreAudioBackendTest.cpp
  audio/backend/CoreAudioProviderTest.cpp
  audio/backend/CoreAudioTestOutputTest.cpp
  audio/backend/detail/CoreAudioChannelLayoutTest.cpp
  audio/backend/detail/CoreAudioControlMailboxTest.cpp
  audio/backend/detail/CoreAudioErrorTest.cpp
  audio/backend/detail/CoreAudioDeviceDiscoveryTest.cpp
  audio/backend/detail/CoreAudioFormatTest.cpp
  audio/backend/detail/CoreAudioGraphTest.cpp
  audio/backend/detail/CoreAudioLatencyTest.cpp
  audio/backend/detail/CoreAudioRenderAdmissionTest.cpp
  audio/backend/detail/CoreAudioRenderBufferTest.cpp
  audio/backend/detail/CoreAudioRunControlTest.cpp
  audio/backend/detail/CoreAudioUnitRunTest.cpp
  audio/backend/detail/CoreFoundationStringTest.cpp
)
if(APPLE)
  target_link_libraries(ao_core_test PRIVATE ao_test_support_coreaudio)
  add_dependencies(ao_core_test ao_coreaudio_output_probe)
endif()
# Only Darwin LMDB keeps lock state that outlives an aborted owner.
aobus_target_test_sources(ao_core_test "${APPLE}" "non-macOS host"
  macos/lmdb/LmdbProbeScratchTest.cpp
)
if(WIN32)
  add_executable(ao_detached_process_probe
    helper/DetachedProcessProbeWindows.cpp
  )
  aobus_enable_windows_utf8_manifest(ao_detached_process_probe)
  target_link_libraries(ao_detached_process_probe PRIVATE shell32)
  add_dependencies(ao_core_test ao_detached_process_probe)

  target_sources(ao_core_test PRIVATE
    audio/backend/WasapiFormatTestSupport.cpp
    ${CMAKE_SOURCE_DIR}/app/windows-winui/platform/WinUiErrorBoundary.cpp
  )
  target_compile_definitions(ao_core_test PRIVATE
    AOBUS_WINDOWS_FRAME_XAML="${CMAKE_SOURCE_DIR}/app/windows-winui/MainWindow.xaml")
  target_link_libraries(ao_core_test PRIVATE advapi32 ao_windows_platform PkgRapidYaml)
endif()
aobus_target_test_sources(ao_core_test "${WIN32}" "non-Windows host"
  audio/backend/WasapiProviderTest.cpp
  audio/backend/WasapiSharedBackendTest.cpp
  audio/backend/WasapiBackendReuseTest.cpp
  audio/backend/detail/WasapiGraphRegistryTest.cpp
  audio/backend/detail/WasapiMixFormatTest.cpp
  audio/backend/detail/WasapiStringsTest.cpp
  windows/platform/MemoryRandomAccessStreamTest.cpp
  winui/FrameResourceTest.cpp
  winui/StringResourceTest.cpp
  winui/WinUiErrorBoundaryTest.cpp
)
add_dependencies(ao_core_test ao_audio_fatal_probe ao_fatal_probe ao_library_probe)
add_test(NAME ao_core_test COMMAND ao_core_test)

if(AOBUS_BUILD_TUI)
add_executable(ao_tui_signal_probe
    fatal/TuiSignalProbeMain.cpp
    fatal/TuiSignalProbeScenario.cpp
)
aobus_enable_windows_utf8_manifest(ao_tui_signal_probe)
target_include_directories(ao_tui_signal_probe PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_tui_signal_probe PRIVATE ao_fatal_probe_support aobus-tui-lib)

# ao_tui_test: terminal frontend unit tests
add_executable(ao_tui_test
    tui/CoverArtTestSupport.cpp
    tui/KeymapTestSupport.cpp
    tui/EventControllerTestSupport.cpp
    tui/RenderTestSupport.cpp
    tui/TrackPropertiesEditorTestSupport.cpp
)
aobus_enable_windows_utf8_manifest(ao_tui_test)

target_link_libraries(ao_tui_test PRIVATE
    ao_app_i18n_ordering
    aobus-tui-lib
    ao_app_uimodel
    ao_app_runtime
    ao_fatal_probe_support
    ao_test_support_i18n
    ao_test_support_runtime
    ao_test_options
    Catch2::Catch2WithMain
    FakeIt::FakeIt-catch
)
add_dependencies(ao_tui_test ao_tui_signal_probe)
add_test(NAME ao_tui_test COMMAND ao_tui_test)

# ao_tui_bench: allocation-bound TUI render micro-benchmarks. EXCLUDE_FROM_ALL so
# it is never built by the gate -- it links mimalloc to mirror the shipping
# aobus-tui allocator, which would collide with the ASan/TSan interceptors that
# --asan/--tsan apply to every in-gate target. Build explicitly and run directly:
#   ./ao build --target ao_tui_bench && <builddir>/test/ao_tui_bench
add_executable(ao_tui_bench EXCLUDE_FROM_ALL)
aobus_enable_windows_utf8_manifest(ao_tui_bench)
target_link_libraries(ao_tui_bench PRIVATE aobus-tui-lib ao_app_uimodel ao_app_runtime ao_test_support_i18n ao_test_options Catch2::Catch2WithMain mimalloc)
endif()
aobus_target_test_sources(ao_tui_test "${AOBUS_BUILD_TUI}" "AOBUS_BUILD_TUI=OFF"
    tui/AnchoredOverlayTest.cpp
    tui/CommandTest.cpp
    tui/CommandCompletionTest.cpp
    tui/CommandCompletionStateTest.cpp
    tui/CoverArtLoaderTest.cpp
    tui/CoverArtTest.cpp
    tui/EventControllerTest.cpp
    tui/EditorMouseTest.cpp
    tui/MouseBindingsTest.cpp
    tui/ExecutorTest.cpp
    tui/ExitControllerTest.cpp
    tui/FrameTimerTest.cpp
    tui/LibraryControllerTest.cpp
    tui/LibraryCreditsCacheTest.cpp
    tui/LibraryNavigationTest.cpp
    tui/LibraryScanControllerTest.cpp
    tui/ListAuthoringControllerTest.cpp
    tui/ListNavigationModelTest.cpp
    tui/ListOrderControllerTest.cpp
    tui/NavigationPanelTest.cpp
    tui/PinnedNavigationTest.cpp
    tui/DetailSectionsTest.cpp
    tui/DetailFieldLayoutTest.cpp
    tui/PanelDividerTest.cpp
    tui/PanelResizeTest.cpp
    tui/PlaybackStatusFormatterTest.cpp
    tui/SelectionNavigationTest.cpp
    tui/SignalExitWatcherTest.cpp
    tui/TrackDetailLinesTest.cpp
    tui/TrackListEntryTest.cpp
    tui/TrackEditControllerTest.cpp
    tui/TrackPresentationNavigationTest.cpp
    tui/TrackCreditsEditorTest.cpp
    tui/TrackPropertiesEditorTest.cpp
    tui/TrackPropertiesEditorCompletionTest.cpp
    tui/TrackTagPopoverTest.cpp
    tui/TrackPropertiesEditorTagsTest.cpp
    tui/SmartListEditorTest.cpp
    tui/TrackSectionTest.cpp
    tui/OutputDeviceControllerTest.cpp
    tui/PlaybackNavigationTest.cpp
    tui/GoToMenuTest.cpp
    tui/PlaybackModeTest.cpp
    tui/PlaybackHoverTest.cpp
    tui/PlaybackPanelTest.cpp
    tui/QualityPanelTest.cpp
    tui/OutputDevicePanelTest.cpp
    tui/RenderTest.cpp
    tui/WorkspaceUsabilityTest.cpp
    tui/ListSearchTest.cpp
    tui/ShellInteractionModelTest.cpp
    tui/SoulButtonTest.cpp
    tui/TerminalTrackColumnLayoutTest.cpp
    tui/TextCellTest.cpp
    tui/TrackTableTest.cpp
    tui/HitRegionsTest.cpp
    tui/KeymapTest.cpp
    tui/SettingsEditorTest.cpp
    tui/PreferencesTest.cpp
    tui/TerminalTitleTest.cpp
    tui/LayoutStateStoreTest.cpp
    tui/TuiSignalProbeTest.cpp
    tui/TextFieldModelTest.cpp
)

# The benchmark remains isolated from ordinary runtime and sanitizer groups,
# but its declarations still require a configured owner or disabled reason.
aobus_target_test_sources(ao_tui_bench "${AOBUS_BUILD_TUI}" "AOBUS_BUILD_TUI=OFF"
    tui/TrackTableBenchmark.cpp
)

if(AOBUS_BUILD_CLI)
# ao_cli_test: command-line frontend tests
add_executable(ao_cli_test
    cli/CliTestSupport.cpp
)
aobus_enable_windows_utf8_manifest(ao_cli_test)

target_link_libraries(ao_cli_test PRIVATE
    aobus-cli-lib
    ao_test_support_audio
    ao_test_support_library
    ao_test_options
    Catch2::Catch2WithMain
)
add_test(NAME ao_cli_test COMMAND ao_cli_test)
endif()
aobus_target_test_sources(ao_cli_test "${AOBUS_BUILD_CLI}" "AOBUS_BUILD_CLI=OFF"
    cli/CommandErrorTest.cpp
    cli/CliRuntimeTest.cpp
    cli/CliSmokeTest.cpp
    cli/InitCommandTest.cpp
    cli/LibCommandTest.cpp
    cli/CueRelinkCommandTest.cpp
    cli/ListCommandTest.cpp
    cli/ListDestructiveCommandTest.cpp
    cli/QueryHelpTest.cpp
    cli/RunTest.cpp
    cli/ScanCommandTest.cpp
    cli/TagCommandTest.cpp
    cli/TrackCommandTest.cpp
    cli/TrackPerformanceMetadataCommandTest.cpp
    cli/TrackLiteralMetadataCommandTest.cpp
    cli/OutputTest.cpp
)

# Native endpoint/service and link-interposition cases. Portable SDK contracts
# stay in core; Windows backend cases are hidden and explicitly selected.
add_executable(ao_backend_test)
aobus_enable_windows_utf8_manifest(ao_backend_test)

target_link_libraries(ao_backend_test PRIVATE
    ao_test_support_audio
    ao_test_options
    Catch2::Catch2WithMain
)
if(LINUX)
  target_link_libraries(ao_backend_test PRIVATE PkgALSA PkgPipeWire)
  # Only the scoped callback-source fake in PipeWireBackendAdmissionTest owns
  # these external calls; every other stream delegates to the native SDK.
  target_link_options(ao_backend_test PRIVATE
    "LINKER:--wrap=pw_stream_add_listener"
    "LINKER:--wrap=pw_stream_connect"
    "LINKER:--wrap=pw_stream_set_active"
    "LINKER:--wrap=pw_stream_dequeue_buffer"
    "LINKER:--wrap=pw_stream_queue_buffer")
endif()
if(APPLE)
  # The native fatal-probe case reenters a live callback in the probe child;
  # siblingProbeExecutablePath() needs this probe beside the test binary.
  target_link_libraries(ao_backend_test PRIVATE ao_fatal_probe_support ao_test_support_coreaudio)
  add_dependencies(ao_backend_test ao_audio_fatal_probe)
endif()
aobus_target_test_sources(ao_backend_test "${LINUX}" "non-Linux host"
  audio/backend/PipeWireBackendAdmissionTest.cpp
  audio/backend/PipeWireChannelObservationTest.cpp
  audio/backend/AlsaChannelObservationTest.cpp
  audio/backend/PipeWireProviderDaemonTest.cpp
)
aobus_target_test_sources(ao_backend_test "${APPLE}" "non-macOS host"
  audio/backend/CoreAudioBackendNativeTest.cpp
  audio/backend/CoreAudioBackendRunStateTest.cpp
  audio/backend/CoreAudioBackendNativeFatalProbeTest.cpp
  audio/backend/detail/CoreAudioDeviceDiscoveryNativeTest.cpp
  audio/backend/CoreAudioChannelObservationTest.cpp
  audio/backend/CoreAudioProviderHardwareTest.cpp
)
aobus_target_test_sources(ao_backend_test "${WIN32}" "non-Windows host"
  audio/backend/WasapiChannelObservationTest.cpp
  audio/backend/WasapiProviderHardwareTest.cpp
)
# Not using add_test() for backend or systemmedia: the portal owns suite
# execution and CTest must not become a second completion gate.

# ao_systemmedia_test: Linux MPRIS surface and real-TUI process contracts.
# Requires Linux system media; the target exists only in that configuration.
# Disabled declarations record ownership reasons in the source manifest. The
# portal checks configured feature flags before selecting an existing binary.
if(LINUX AND AOBUS_BUILD_SYSTEM_MEDIA)
  add_executable(ao_systemmedia_test)
  aobus_enable_windows_utf8_manifest(ao_systemmedia_test)

  target_link_libraries(ao_systemmedia_test PRIVATE
      ao_app_uimodel
      ao_test_support_library
      ao_test_options
      Catch2::Catch2WithMain
      FakeIt::FakeIt-catch
  )
  target_sources(ao_systemmedia_test PRIVATE systemmedia/linux/MprisTestSupport.cpp)
  target_link_libraries(ao_systemmedia_test PRIVATE
      ao_system_media_linux
      ao_test_support_runtime
      PkgGIOMM
      PkgGLIBMM
  )
endif()
set(AOBUS_TEST_SYSTEM_MEDIA_ENABLED OFF)
if(LINUX AND AOBUS_BUILD_SYSTEM_MEDIA)
  set(AOBUS_TEST_SYSTEM_MEDIA_ENABLED ON)
endif()
aobus_target_test_sources(ao_systemmedia_test "${AOBUS_TEST_SYSTEM_MEDIA_ENABLED}" "Linux system media is disabled or unavailable"
  systemmedia/linux/MprisArtUrlSessionTest.cpp
  systemmedia/linux/MprisBridgeTest.cpp
  systemmedia/linux/MprisBridgeIntegrationTest.cpp
  systemmedia/linux/MprisBusSessionTest.cpp
  systemmedia/linux/MprisSignalTest.cpp
  systemmedia/linux/MprisPlaybackPositionTest.cpp
)
# Real TUI process coverage is normal/ASan only; FTXUI is outside supported TSan suites.
set(AOBUS_TEST_TUI_SYSTEM_MEDIA_ENABLED OFF)
if(AOBUS_TEST_SYSTEM_MEDIA_ENABLED AND AOBUS_BUILD_TUI AND NOT AOBUS_ENABLE_TSAN)
  set(AOBUS_TEST_TUI_SYSTEM_MEDIA_ENABLED ON)
  target_compile_definitions(ao_systemmedia_test PRIVATE AOBUS_TUI_EXECUTABLE="$<TARGET_FILE:aobus-tui>")
  add_dependencies(ao_systemmedia_test aobus-tui)
endif()
aobus_target_test_sources(ao_systemmedia_test "${AOBUS_TEST_TUI_SYSTEM_MEDIA_ENABLED}" "Linux system media or TUI unavailable, or unsupported TUI TSan coverage"
  systemmedia/linux/TuiMprisProcessTest.cpp
)


if(AOBUS_BUILD_GTK)
# ao_gtk_test: Linux UI tests (require aobus-gtk-lib and GTKMM)
add_executable(ao_gtk_test
    linux-gtk/GtkTestMain.cpp
)

target_link_libraries(ao_gtk_test PRIVATE
    aobus-gtk-lib
    ao_app_i18n_ordering
    ao_fatal_probe_support
    ao_test_support_gtk
    ao_test_support_runtime
    PkgGTKMM
    PkgConfig::XTEST
    ao_test_options
    Catch2::Catch2
    FakeIt::FakeIt-catch
)
add_dependencies(ao_gtk_test ao_gapplication_probe)
if(LINUX AND AOBUS_BUILD_SYSTEM_MEDIA)
    target_compile_definitions(ao_gtk_test PRIVATE AOBUS_HAS_SYSTEM_MEDIA=1)
    target_link_libraries(ao_gtk_test PRIVATE ao_system_media_linux)
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(ao_gtk_test PRIVATE -Wl,-u,aobus_get_resource)
endif()
add_test(NAME ao_gtk_test COMMAND ao_gtk_test)
set_tests_properties(ao_gtk_test PROPERTIES
    ENVIRONMENT "GTK_A11Y=test;GTK_IM_MODULE=simple;GDK_BACKEND=x11;GDK_DISABLE=gl,vulkan;GSK_RENDERER=cairo"
)
endif()
aobus_target_test_sources(ao_gtk_test "${AOBUS_BUILD_GTK}" "AOBUS_BUILD_GTK=OFF"
    linux-gtk/GtkTestEnvironmentTest.cpp
    linux-gtk/common/ActionMapRegistrationTest.cpp
    linux-gtk/common/DismissControllerTest.cpp
    linux-gtk/common/MainContextCallbackScopeTest.cpp
    linux-gtk/common/PopoverAttachmentTest.cpp
    linux-gtk/common/UiWorkflowTest.cpp
    linux-gtk/completion/EntryCompletionControllerTest.cpp
    linux-gtk/list/ListModelTest.cpp
    linux-gtk/list/ListTreeModelBuilderTest.cpp
    linux-gtk/list/QueryExpressionBoxTest.cpp
    linux-gtk/list/SmartListDialogTest.cpp
    linux-gtk/list/ListNavigationControllerTest.cpp
    linux-gtk/tag/TagEditorTest.cpp
    linux-gtk/tag/TagEditControllerTest.cpp
    linux-gtk/tag/TrackPropertiesDialogTest.cpp
    linux-gtk/tag/TrackRecordingDatePropertiesTest.cpp
    linux-gtk/layout/components/TrackCreditsEditorTest.cpp
    linux-gtk/layout/components/TrackCreditsRefreshTest.cpp
    linux-gtk/image/ImageWidgetTest.cpp
    linux-gtk/image/ImageRenderPolicyTest.cpp
    linux-gtk/image/ImageCacheTest.cpp
    linux-gtk/image/ResourceImageLoaderTest.cpp
    linux-gtk/app/AppDialogTest.cpp
    linux-gtk/app/FormBuilderTest.cpp
    linux-gtk/app/ThemeCoordinatorTest.cpp
    linux-gtk/app/ThemePresetTest.cpp
    linux-gtk/app/MainWindowSessionPresentationTest.cpp
    linux-gtk/app/MenuControllerTest.cpp
    linux-gtk/i18n/GtkTextTest.cpp
    linux-gtk/app/GtkAccelTranslatorTest.cpp
    linux-gtk/app/KeymapApplicatorTest.cpp
    linux-gtk/preference/ShortcutEditorWidgetTest.cpp
    linux-gtk/app/WindowInputTest.cpp
    linux-gtk/app/AobusSoulTest.cpp
    linux-gtk/app/GtkStyleRuntimeTest.cpp
    linux-gtk/app/GtkMainContextExecutorTest.cpp
    linux-gtk/app/GtkStartupPlanTest.cpp
    linux-gtk/app/AppConfigStoreTest.cpp
    linux-gtk/app/GApplicationReplacementTest.cpp
    linux-gtk/app/MainWindowTest.cpp
    linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp
    linux-gtk/app/ShellLayoutStoreTest.cpp
    linux-gtk/app/ShellLayoutControllerTest.cpp
    linux-gtk/app/GtkLayoutStateStoreTest.cpp
    linux-gtk/app/WindowStateTest.cpp
    linux-gtk/preference/PreferencesWindowTest.cpp
    linux-gtk/track/TrackListModelTest.cpp
    linux-gtk/track/TrackSelectionControllerTest.cpp
    linux-gtk/track/TrackSelectionControllerNativeInputTest.cpp
    linux-gtk/track/TrackPageHostTest.cpp
    linux-gtk/track/TrackViewPageTest.cpp
    linux-gtk/track/TrackColumnControllerTest.cpp
    linux-gtk/track/TrackColumnFactoryBuilderTest.cpp
    linux-gtk/track/TrackQuickFilterTest.cpp
    linux-gtk/track/TrackPresentationButtonTest.cpp
    linux-gtk/track/TrackCustomViewDialogTest.cpp
    linux-gtk/track/TrackFieldUiTest.cpp
    linux-gtk/track/TrackRowCacheTest.cpp
    linux-gtk/track/TrackRowObjectTest.cpp
    linux-gtk/track/SelectionInfoLabelTest.cpp
    linux-gtk/status/ActivityStatusWidgetTest.cpp
    linux-gtk/portal/ImportExportCoordinatorTest.cpp
    linux-gtk/portal/LibraryImportExportWorkflowTest.cpp
    linux-gtk/platform/SuccessorProcessLauncherTest.cpp
      linux-gtk/layout/document/LayoutModelTest.cpp
      linux-gtk/layout/runtime/ActionRegistryTest.cpp
      linux-gtk/layout/runtime/ActionBinderTest.cpp
      linux-gtk/layout/runtime/ComponentInteractionControllerTest.cpp
      linux-gtk/layout/runtime/ComponentRegistryInjectionTest.cpp
      linux-gtk/layout/runtime/GioActionBridgeTest.cpp
      linux-gtk/layout/components/LayoutRuntimeBuildTest.cpp
      linux-gtk/layout/components/ContainerErrorStatesTest.cpp
      linux-gtk/layout/components/SplitComponentTest.cpp
      linux-gtk/layout/components/CollapsibleSplitComponentTest.cpp
      linux-gtk/layout/components/CenterBoxTest.cpp
      linux-gtk/layout/components/ResponsiveClassTest.cpp
      linux-gtk/layout/components/ScrollSeparatorTabsTest.cpp
      linux-gtk/layout/components/ApplyCommonPropsTest.cpp
      linux-gtk/layout/components/LayoutHostTest.cpp
      linux-gtk/layout/components/LayoutComponentsTest.cpp
      linux-gtk/layout/components/PlaybackLayoutComponentsTest.cpp
      linux-gtk/layout/components/SemanticLayoutComponentsTest.cpp
      linux-gtk/layout/components/LayoutComponentYamlTest.cpp
      linux-gtk/layout/components/TrackFieldGridCollapsibleTest.cpp
      linux-gtk/layout/components/TrackDetailConstrainedLayoutTest.cpp
      linux-gtk/layout/components/TrackFieldGridTextTest.cpp
      linux-gtk/layout/components/TrackSelectionRegionTest.cpp
      linux-gtk/layout/components/PlaybackImageTest.cpp
      linux-gtk/layout/editor/LayoutEditorTest.cpp
      linux-gtk/layout/editor/LayoutEditorTextTest.cpp
      linux-gtk/layout/editor/LayoutEditorAbsoluteCanvasTest.cpp
      linux-gtk/layout/editor/LayoutEditorSchemaTest.cpp
      linux-gtk/layout/editor/LayoutEditorDialogSessionTest.cpp
      linux-gtk/layout/editor/LayoutEditorTemplateTest.cpp
      linux-gtk/layout/components/StatusComponentsTest.cpp
    linux-gtk/playback/AudioPipelinePanelTest.cpp
    linux-gtk/playback/AobusSoulWindowTest.cpp
    linux-gtk/playback/OutputDevicePopoverTest.cpp
    linux-gtk/playback/PlaybackUiComponentsTest.cpp
    linux-gtk/playback/PlaybackDetailsWidgetTest.cpp
    linux-gtk/playback/NowPlayingFieldLabelTest.cpp
    linux-gtk/playback/NowPlayingStatusLabelTest.cpp
    linux-gtk/playback/OutputDeviceListItemsTest.cpp
    linux-gtk/playback/TransportButtonTest.cpp
    linux-gtk/playback/VolumeControlWidgetTest.cpp
)

# ao_perf_baseline: standalone performance baseline (not run by default in ./ao check)
add_executable(ao_perf_baseline EXCLUDE_FROM_ALL
    perf/baseline/PerformanceMain.cpp
    perf/baseline/ClassicalMetadataBaselineTest.cpp
    perf/baseline/CreditsBaselineTest.cpp
    perf/baseline/PerformanceMetadataBaselineTest.cpp
    perf/baseline/PerformanceMetadataIncrementalBaselineTest.cpp
    perf/baseline/CompletionVocabularyBaselineTest.cpp
    perf/baseline/PerformanceReviewTest.cpp
    perf/baseline/PerformanceBaselineTest.cpp
)
aobus_enable_windows_utf8_manifest(ao_perf_baseline)

target_link_libraries(ao_perf_baseline PRIVATE
    ICU::uc
    ao_test_support_performance
    ao_app_i18n_completion
    ao_app_i18n_ordering
    ao_app_uimodel
    ao_test_support_runtime
    ao_test_options
    Catch2::Catch2
    FakeIt::FakeIt-catch
)

# Compilation database support for custom linter integration fixture
file(GLOB_RECURSE LINT_FIXTURE_SOURCES "lint/fixture/*.cpp")
add_library(ao_lint_fixture OBJECT EXCLUDE_FROM_ALL ${LINT_FIXTURE_SOURCES})
target_include_directories(ao_lint_fixture PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/test/lint/fixture")

aobus_finalize_test_sources(
  "${CMAKE_BINARY_DIR}/aobus-test-sources.tsv"
  ao_core_test ao_backend_test ao_systemmedia_test ao_tui_test ao_cli_test ao_gtk_test ao_perf_baseline ao_tui_bench
)
