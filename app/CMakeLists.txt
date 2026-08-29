# Interactive localization is a leaf application capability. Its build-time
# compiler and ICU implementation stay out of Core, runtime, and the CLI graph.
add_library(ao_catalog_pattern STATIC
    i18n/CatalogPattern.cpp
)
target_include_directories(ao_catalog_pattern PUBLIC "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(ao_catalog_pattern PRIVATE ICU::i18n ICU::uc ICU::data)

add_executable(ao_catalog_compiler
    ${CMAKE_SOURCE_DIR}/tool/catalog/CatalogCompiler.cpp
)
aobus_enable_windows_utf8_manifest(ao_catalog_compiler)
target_include_directories(ao_catalog_compiler PRIVATE "${CMAKE_SOURCE_DIR}/app/include")
target_link_libraries(ao_catalog_compiler PRIVATE ao_catalog_pattern ao_utility)

set(AOBUS_CATALOG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/i18n/catalog")
set(AOBUS_CATALOG_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/i18n/catalog")
set(AOBUS_CATALOG_RESW_DIR "${CMAKE_CURRENT_BINARY_DIR}/i18n/resw")
file(MAKE_DIRECTORY
    "${AOBUS_CATALOG_GENERATED_DIR}"
    "${AOBUS_CATALOG_RESW_DIR}/en"
    "${AOBUS_CATALOG_RESW_DIR}/de"
    "${AOBUS_CATALOG_RESW_DIR}/zh-Hans"
    "${AOBUS_CATALOG_RESW_DIR}/zh-Hant"
    "${AOBUS_CATALOG_RESW_DIR}/ja"
    "${AOBUS_CATALOG_RESW_DIR}/es"
    "${AOBUS_CATALOG_RESW_DIR}/fr"
    "${AOBUS_CATALOG_RESW_DIR}/qps-ploc")

set(AOBUS_CATALOG_ROOT_RES "${AOBUS_CATALOG_GENERATED_DIR}/root.res")
set(AOBUS_CATALOG_DE_RES "${AOBUS_CATALOG_GENERATED_DIR}/de.res")
set(AOBUS_CATALOG_ZH_HANS_RES "${AOBUS_CATALOG_GENERATED_DIR}/zh_Hans.res")
set(AOBUS_CATALOG_ZH_HANT_RES "${AOBUS_CATALOG_GENERATED_DIR}/zh_Hant.res")
set(AOBUS_CATALOG_JA_RES "${AOBUS_CATALOG_GENERATED_DIR}/ja.res")
set(AOBUS_CATALOG_ES_RES "${AOBUS_CATALOG_GENERATED_DIR}/es.res")
set(AOBUS_CATALOG_FR_RES "${AOBUS_CATALOG_GENERATED_DIR}/fr.res")

set(AOBUS_CATALOG_PSEUDO_SOURCE "${AOBUS_CATALOG_GENERATED_DIR}/qps_Ploc.txt")
set(AOBUS_CATALOG_PSEUDO_RES "${AOBUS_CATALOG_GENERATED_DIR}/qps_Ploc.res")
set(AOBUS_CATALOG_DATA "${AOBUS_CATALOG_GENERATED_DIR}/aobus_messages.dat")
set(AOBUS_CATALOG_EMBEDDED_CPP "${AOBUS_CATALOG_GENERATED_DIR}/EmbeddedCatalogData.cpp")

set(AOBUS_CATALOG_EN_RESW "${AOBUS_CATALOG_RESW_DIR}/en/Resources.resw")
set(AOBUS_CATALOG_DE_RESW "${AOBUS_CATALOG_RESW_DIR}/de/Resources.resw")
set(AOBUS_CATALOG_ZH_HANS_RESW "${AOBUS_CATALOG_RESW_DIR}/zh-Hans/Resources.resw")
set(AOBUS_CATALOG_ZH_HANT_RESW "${AOBUS_CATALOG_RESW_DIR}/zh-Hant/Resources.resw")
set(AOBUS_CATALOG_JA_RESW "${AOBUS_CATALOG_RESW_DIR}/ja/Resources.resw")
set(AOBUS_CATALOG_ES_RESW "${AOBUS_CATALOG_RESW_DIR}/es/Resources.resw")
set(AOBUS_CATALOG_FR_RESW "${AOBUS_CATALOG_RESW_DIR}/fr/Resources.resw")
set(AOBUS_CATALOG_PSEUDO_RESW "${AOBUS_CATALOG_RESW_DIR}/qps-ploc/Resources.resw")

set(AOBUS_CATALOG_WINUI_RESW_FILES
    "${AOBUS_CATALOG_EN_RESW}"
    "${AOBUS_CATALOG_DE_RESW}"
    "${AOBUS_CATALOG_ZH_HANS_RESW}"
    "${AOBUS_CATALOG_ZH_HANT_RESW}"
    "${AOBUS_CATALOG_JA_RESW}"
    "${AOBUS_CATALOG_ES_RESW}"
    "${AOBUS_CATALOG_FR_RESW}"
    "${AOBUS_CATALOG_PSEUDO_RESW}")

add_custom_command(
    OUTPUT
            "${AOBUS_CATALOG_ROOT_RES}"
            "${AOBUS_CATALOG_DE_RES}"
            "${AOBUS_CATALOG_ZH_HANS_RES}"
            "${AOBUS_CATALOG_ZH_HANT_RES}"
            "${AOBUS_CATALOG_JA_RES}"
            "${AOBUS_CATALOG_ES_RES}"
            "${AOBUS_CATALOG_FR_RES}"
    COMMAND "${AOBUS_ICU_GENRB_EXECUTABLE}"
            --strict
            --encoding UTF-8
            --sourcedir "${AOBUS_CATALOG_SOURCE_DIR}"
            --destdir "${AOBUS_CATALOG_GENERATED_DIR}"
            root.txt de.txt zh_Hans.txt zh_Hant.txt ja.txt es.txt fr.txt
    DEPENDS
            "${AOBUS_CATALOG_SOURCE_DIR}/root.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/de.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/zh_Hans.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/zh_Hant.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/ja.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/es.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/fr.txt"
    COMMENT "Compiling canonical ICU message catalogs"
    VERBATIM)

add_custom_command(
    OUTPUT
            "${AOBUS_CATALOG_PSEUDO_SOURCE}"
            ${AOBUS_CATALOG_WINUI_RESW_FILES}
    COMMAND $<TARGET_FILE:ao_catalog_compiler>
            --root-res root.res
            --pseudo-source qps_Ploc.txt
            --en-resw ../resw/en/Resources.resw
            --pseudo-resw ../resw/qps-ploc/Resources.resw
            --translation de.res ../resw/de/Resources.resw
            --translation zh_Hans.res ../resw/zh-Hans/Resources.resw
            --translation zh_Hant.res ../resw/zh-Hant/Resources.resw
            --translation ja.res ../resw/ja/Resources.resw
            --translation es.res ../resw/es/Resources.resw
            --translation fr.res ../resw/fr/Resources.resw
    DEPENDS ao_catalog_compiler
            "${AOBUS_CATALOG_ROOT_RES}"
            "${AOBUS_CATALOG_DE_RES}"
            "${AOBUS_CATALOG_ZH_HANS_RES}"
            "${AOBUS_CATALOG_ZH_HANT_RES}"
            "${AOBUS_CATALOG_JA_RES}"
            "${AOBUS_CATALOG_ES_RES}"
            "${AOBUS_CATALOG_FR_RES}"
    WORKING_DIRECTORY "${AOBUS_CATALOG_GENERATED_DIR}"
    COMMENT "Validating catalogs and generating pseudo/WinUI resources"
    VERBATIM)

add_custom_command(
    OUTPUT "${AOBUS_CATALOG_PSEUDO_RES}"
    COMMAND "${AOBUS_ICU_GENRB_EXECUTABLE}"
            --strict
            --encoding UTF-8
            --sourcedir "${AOBUS_CATALOG_GENERATED_DIR}"
            --destdir "${AOBUS_CATALOG_GENERATED_DIR}"
            qps_Ploc.txt
    DEPENDS "${AOBUS_CATALOG_PSEUDO_SOURCE}"
    COMMENT "Compiling generated pseudo-localization catalog"
    VERBATIM)

add_custom_command(
    OUTPUT "${AOBUS_CATALOG_DATA}"
    COMMAND "${AOBUS_ICU_PKGDATA_EXECUTABLE}"
            --quiet
            --force-prefix
            --name aobus_messages
            --mode common
            --sourcedir "${AOBUS_CATALOG_GENERATED_DIR}"
            --destdir "${AOBUS_CATALOG_GENERATED_DIR}"
            --tempdir "${AOBUS_CATALOG_GENERATED_DIR}"
            "${AOBUS_CATALOG_SOURCE_DIR}/package.lst"
    DEPENDS
            "${AOBUS_CATALOG_ROOT_RES}"
            "${AOBUS_CATALOG_DE_RES}"
            "${AOBUS_CATALOG_ZH_HANS_RES}"
            "${AOBUS_CATALOG_ZH_HANT_RES}"
            "${AOBUS_CATALOG_JA_RES}"
            "${AOBUS_CATALOG_ES_RES}"
            "${AOBUS_CATALOG_FR_RES}"
            "${AOBUS_CATALOG_PSEUDO_RES}"
            "${AOBUS_CATALOG_SOURCE_DIR}/package.lst"
    COMMENT "Packaging embedded ICU message data"
    VERBATIM)

add_custom_command(
    OUTPUT "${AOBUS_CATALOG_EMBEDDED_CPP}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${AOBUS_CATALOG_DATA}"
            "-DOUTPUT=${AOBUS_CATALOG_EMBEDDED_CPP}"
            -P "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
    DEPENDS "${AOBUS_CATALOG_DATA}" "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
    COMMENT "Embedding ICU message data"
    VERBATIM)

add_library(ao_app_i18n STATIC
    i18n/MessageCatalog.cpp
    i18n/EmbeddedCatalogData.h
    i18n/MessageIds.h
    i18n/SystemLocale.h
    "${AOBUS_CATALOG_EMBEDDED_CPP}"
)
if(WIN32)
  target_sources(ao_app_i18n PRIVATE i18n/SystemLocaleWindows.cpp)
else()
  target_sources(ao_app_i18n PRIVATE i18n/SystemLocalePosix.cpp)
endif()
target_include_directories(ao_app_i18n
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ao_app_i18n
    PRIVATE ao_catalog_pattern ao_unicode_text ao_utility ICU::i18n ICU::uc ICU::data)

# Locale collation stays in an interactive leaf. The policy interface itself
# contains no ICU types, so runtime/UIModel consumers do not inherit ICU::i18n.
add_library(ao_app_i18n_ordering STATIC
    i18n/IcuTextOrdering.cpp
)
target_include_directories(ao_app_i18n_ordering
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_i18n_ordering
    PRIVATE ao_unicode_text ICU::i18n ICU::uc ICU::data)

# Completion aliases use the same ICU leaf boundary as locale ordering. The
# runtime policy carries no ICU types and the CLI must not reach this target.
add_library(ao_app_i18n_completion STATIC
    i18n/IcuCompletionAliases.cpp
)
target_include_directories(ao_app_i18n_completion
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(ao_app_i18n_completion
    PRIVATE ao_unicode_text ICU::i18n ICU::uc ICU::data)

# Prepared-next contract diagnostics are compiled once and shared with the
# minimal fatal probe so source-location coverage does not duplicate a source
# file across targets with incompatible compile commands.
add_library(ao_prepared_next_contract STATIC
    runtime/playback/PreparedNextContract.cpp
)
target_include_directories(ao_prepared_next_contract PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_prepared_next_contract PRIVATE ao_utility)

# ao_app_desktop: shared pure desktop lifecycle rules and process mechanism.
# Toolkit registration, windows, dispatchers, stores, and diagnostics stay in
# the GTK and WinUI composition roots.
add_library(ao_app_desktop STATIC
    desktop/DetachedProcessLauncher.cpp
    desktop/LibraryPath.cpp
    desktop/LibraryStartupPlanner.cpp
    desktop/LibrarySuccessorProtocol.cpp
    desktop/LibrarySwitch.cpp
)
target_include_directories(ao_app_desktop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ao_app_desktop PUBLIC ao_utility)
target_link_libraries(ao_app_desktop PRIVATE Boost::process)

# app/runtime: Shared application runtime (UI-neutral, used by all frontends)
add_library(ao_app_runtime STATIC
    runtime/AppRuntime.cpp
    runtime/AppState.cpp
    runtime/CoreRuntime.cpp
    runtime/library/Library.cpp
    runtime/library/LibraryPaths.cpp
    runtime/library/LibraryWriteLane.cpp
    runtime/library/LibrarySnapshot.cpp
    runtime/library/LibraryChanges.cpp
    runtime/library/MediaTrack.cpp
    runtime/library/TrackBuilderSnapshot.cpp
    runtime/library/LibraryYamlImporter.cpp
    runtime/library/LibraryYamlExporter.cpp
    runtime/completion/CompletionService.cpp
    runtime/completion/CompletionText.cpp
    runtime/completion/QueryExpressionCompleter.cpp
    runtime/completion/MetadataValueCompleter.cpp
  runtime/source/AllTracksSource.cpp
  runtime/source/IndexedTrackSequence.cpp
    runtime/source/CachedListSource.cpp
    runtime/ConfigStore.cpp
    runtime/playback/PlaybackTransport.cpp
    runtime/playback/PlaybackSuccession.cpp
    runtime/PlaybackSessionYamlSchema.cpp
    runtime/PlaybackSessionPersistence.cpp
    runtime/PlaybackSessionState.cpp
    runtime/playback/PlaybackService.cpp
    runtime/playback/PlaybackCursor.cpp
    runtime/playback/PlaybackCursorSession.cpp
    runtime/playback/PlaybackRestartDeadline.cpp
    runtime/playback/PreparedNextRegistry.cpp
    runtime/playback/ProjectionAnchor.cpp
    runtime/playback/ShuffleHistory.cpp
    runtime/library/LibraryCommands.cpp
    runtime/library/AudioIdentityBatchWriter.cpp
    runtime/library/AudioIdentityIndexer.cpp
    runtime/library/LibraryScan.cpp
    runtime/library/ScanPlan.cpp
    runtime/library/ScanApplyOperation.cpp
    runtime/library/LibraryJobs.cpp
    runtime/resource/ResourceByteMemoryCache.cpp
    runtime/resource/ResourceCarrierIndex.cpp
    runtime/resource/ResourceByteDiskCache.cpp
    runtime/resource/ResourceByteReader.cpp
    runtime/source/TrackSourceCache.cpp
    runtime/source/ListOrderSource.cpp
    runtime/NavigationHistory.cpp
    runtime/WorkspaceSessionYamlSchema.cpp
    runtime/WorkspaceService.cpp
    runtime/NotificationService.cpp
    runtime/WritableTagList.cpp
    runtime/source/SmartListEvaluator.cpp
    runtime/source/SmartListSource.cpp
    runtime/TrackPresentation.cpp
    runtime/TrackField.cpp
    runtime/TrackFieldReader.cpp
    runtime/TrackEditScript.cpp
    runtime/projection/TrackListProjection.cpp
    runtime/projection/TrackProjectionEditScript.cpp
    runtime/projection/TrackDetailProjection.cpp
    runtime/source/TrackSource.cpp
    runtime/source/TrackSourceDelta.cpp
    runtime/source/TrackSourceDeltaBuilder.cpp
    runtime/source/TrackSourceLease.cpp
    runtime/ScopedTimer.cpp
    runtime/ViewService.cpp
    runtime/Log.cpp
)

# GCC 15 misclassifies Boost.Asio coroutine-frame aligned_alloc/free pairs in
# these coroutine-heavy translation units after inlining. Keep the workaround
# local so unrelated runtime and frontend code retains the warning.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  set_property(
    SOURCE
      runtime/library/LibraryJobs.cpp
      runtime/library/LibraryCommands.cpp
    APPEND PROPERTY COMPILE_OPTIONS -Wno-mismatched-new-delete)
endif()

target_include_directories(ao_app_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ao_app_runtime PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_runtime PUBLIC ao ao_async PkgRapidYaml spdlog::spdlog)
target_link_libraries(ao_app_runtime PRIVATE ao_prepared_next_contract ao_unicode_text Boost::headers)

if(WIN32)
  add_library(ao_windows_platform STATIC
    windows/MemoryRandomAccessStream.cpp
    windows/include/ao/winui/MemoryRandomAccessStream.h
  )
  target_include_directories(ao_windows_platform PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/windows/include")
  target_link_libraries(ao_windows_platform PUBLIC ole32 runtimeobject shcore)
  if(AOBUS_BUILD_WINUI)
    aobus_enable_cppwinrt(ao_windows_platform)
  endif()
endif()

# ao_app_uimodel: platform-neutral UI models (reusable by linux-gtk, WinUI 3, tests)
add_library(ao_app_uimodel STATIC
    uimodel/presentation/CoverArtPlaceholder.cpp
    uimodel/presentation/PresentationText.cpp
    uimodel/layout/component/LayoutSchema.cpp
    uimodel/layout/shell/LayoutSession.cpp
    uimodel/layout/component/AbsoluteCanvasGeometry.cpp
    uimodel/layout/component/LayoutComponentState.cpp
    uimodel/layout/component/LayoutStatePromoter.cpp
    uimodel/layout/document/LayoutDocument.cpp
    uimodel/layout/document/LayoutDocumentLoader.cpp
    uimodel/layout/document/LayoutNodeId.cpp
    uimodel/layout/document/LayoutPlacement.cpp
    uimodel/layout/document/LayoutPreparation.cpp
    uimodel/layout/document/LayoutValidation.cpp
    uimodel/layout/document/LayoutValue.cpp
    uimodel/layout/shell/ShellGenerationSequence.cpp
    uimodel/input/KeyChord.cpp
    uimodel/input/KeymapModel.cpp
    uimodel/input/KeyRepeatGuard.cpp
    uimodel/input/KeymapStore.cpp
    uimodel/preference/PreferencesEditorModel.cpp
    uimodel/field/TrackFieldFormatter.cpp
    uimodel/library/detail/TrackCustomMetadata.cpp
    uimodel/library/detail/TrackFieldGrid.cpp
    uimodel/library/list/ListActions.cpp
    uimodel/library/list/ListAuthoring.cpp
    uimodel/library/list/ListOrderAuthoringSession.cpp
    uimodel/library/list/ListOrder.cpp
    uimodel/library/list/ListTreeProjection.cpp
    uimodel/library/list/SmartListEditing.cpp
    uimodel/library/presentation/TrackColumnLayoutYamlSchema.cpp
    uimodel/library/presentation/TrackColumnLayouts.cpp
    uimodel/library/presentation/TrackColumnWidthSolver.cpp
    uimodel/library/presentation/CustomPresentationEditorModel.cpp
    uimodel/library/presentation/TrackGroupHeadingPresentation.cpp
    uimodel/library/presentation/TrackColumnDefaults.cpp
    uimodel/library/presentation/TrackPresentationCatalog.cpp
    uimodel/library/presentation/ListPresentationPreferenceYamlSchema.cpp
    uimodel/library/presentation/ListPresentations.cpp
    uimodel/library/presentation/ListPresentationRecommendation.cpp
    uimodel/library/presentation/TrackPresentationPickerViewModel.cpp
    uimodel/library/property/TagEdit.cpp
    uimodel/library/property/TrackPropertiesFormSpec.cpp
    uimodel/library/property/TrackPropertiesFormModel.cpp
    uimodel/library/task/LibraryScanOutcome.cpp
    uimodel/library/task/LibraryScanWorkflow.cpp
    uimodel/library/track/ListMembershipAuthoringSession.cpp
    uimodel/library/track/TrackAuthoring.cpp
    uimodel/library/track/TrackAuthoringSession.cpp
    uimodel/library/track/TrackSelectionSummary.cpp
    uimodel/library/track/TrackFilterPolicy.cpp
    uimodel/library/track/TrackFilterCompleter.cpp
    uimodel/library/track/TrackFilter.cpp
    uimodel/library/track/TrackFilterViewModel.cpp
    uimodel/library/track/TrackPageRoute.cpp
    uimodel/library/track/IndexedTrackRowCache.cpp
    uimodel/library/track/TrackDisplayIndex.cpp
    uimodel/playback/command/PlaybackCommand.cpp
    uimodel/playback/command/PlaybackActions.cpp
    uimodel/playback/now-playing/NowPlayingViewModel.cpp
    uimodel/playback/output/OutputDeviceIntent.cpp
    uimodel/playback/output/OutputSelection.cpp
    uimodel/playback/output/OutputDeviceViewModel.cpp
    uimodel/playback/output/VolumeViewModel.cpp
    uimodel/playback/quality/AudioQualityFormatter.cpp
    uimodel/playback/seek/PlaybackPositionInterpolator.cpp
    uimodel/playback/seek/PlaybackPosition.cpp
    uimodel/playback/seek/PlaybackTimeFormatter.cpp
    uimodel/playback/seek/SeekInteraction.cpp
    uimodel/playback/soul/AobusSoulViewModel.cpp
    uimodel/playback/transport/TransportViewModel.cpp
    uimodel/status/activity/ActivityStatusFeedProjection.cpp
    uimodel/status/activity/ActivityStatusViewModel.cpp
)
target_include_directories(ao_app_uimodel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ao_app_uimodel PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_uimodel PUBLIC ao_app_runtime)
target_link_libraries(ao_app_uimodel PRIVATE ao_app_i18n)

# --- Boundary guardrails for ao_app_uimodel ---
# Ensure shared UI-model layout files never include platform UI headers
add_custom_target(ao_uimodel_layout_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/uimodel/layout"
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/uimodel/layout"
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking for forbidden platform UI includes in ao_app_uimodel layout files"
  VERBATIM
)

add_custom_target(ao_uimodel_organization_guardrail
  COMMAND ${CMAKE_COMMAND}
          "-DPUBLIC_ROOT=${CMAKE_SOURCE_DIR}/app/include/ao/uimodel"
          "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}/app/uimodel"
          "-DTEST_ROOT=${CMAKE_SOURCE_DIR}/test/unit/uimodel"
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertUimodelOrganization.cmake
  COMMENT "Checking UIModel namespace and folder organization"
  VERBATIM
)

# A frontend's own vocabulary is not a shared model, however portable its code
# is. The guardrail names the frontends rather than a spelling of "platform"
# because a frontend-owned LayoutSchema.h passes every include check there is:
# it has no platform includes to find.
add_custom_target(ao_uimodel_frontend_vocabulary_guardrail
  COMMAND ${CMAKE_COMMAND}
          "-DPUBLIC_ROOT=${CMAKE_SOURCE_DIR}/app/include/ao/uimodel"
          "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}/app/uimodel"
          "-DTEST_ROOT=${CMAKE_SOURCE_DIR}/test/unit/uimodel"
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertUimodelFrontendNeutrality.cmake
  COMMENT "Checking no frontend names a file inside ao_app_uimodel"
  VERBATIM
)

# --- Layering boundary guardrails ---
# Cleared boundaries run in enforce mode from the completion-gate aggregate.
# ao_layering_boundary_report remains a focused application-layer entry point.
set(AO_FORBIDDEN_AUDIO_CONTROL_INCLUDE_REGEX "ao/audio/(Player|Backend\\.h|BackendProvider\\.h|Engine|NullBackend|backend/|detail/)")
set(AO_FORBIDDEN_WRITE_AUTHORITY_REGEX "(^|[^A-Za-z0-9_])(WriteTransaction|WritableMusicLibrary)([^A-Za-z0-9_]|$)")
set(AO_RUNTIME_WRITE_TRANSACTION_REGEX "(^|[^A-Za-z0-9_])writeTransaction[ \t\r\n]*\\(")
set(AO_RUNTIME_WRITE_TRANSACTION_OWNER_REGEX
  "/app/runtime/library/(LibraryWriteLane|LibraryYamlImporter|ScanApplyOperation)[.]cpp$")
set(AO_FORBIDDEN_FRONTEND_LIBRARY_PATH_REGEX "\"([.]aobus|data[.]mdb)")
# CMake regexes have no word-boundary escape, so delimit the exact legacy
# vocabulary without rejecting adjacent type or namespace names.
set(AO_FORBIDDEN_APPLICATION_SIGNAL_PRIMITIVE_REGEX
  "#[ \t]*include[ \t]*[<\\\"]ao/rt/(Signal|Subscription)[.]h[>\\\"]|(^|[^A-Za-z0-9_:])((::)?ao::rt::|rt::)(Signal|Subscription)([^A-Za-z0-9_]|$)")
set(AO_FORBIDDEN_INTERNAL_PLAYBACK_REGEX
  "(^|[^A-Za-z0-9_])(PlaybackTransport|PlaybackSuccession)([^A-Za-z0-9_]|$)")

foreach(AO_LEGACY_SIGNAL_SPELLING IN ITEMS "rt::Signal" "ao::rt::Subscription" "::ao::rt::Signal")
  if(NOT "${AO_LEGACY_SIGNAL_SPELLING}" MATCHES "${AO_FORBIDDEN_APPLICATION_SIGNAL_PRIMITIVE_REGEX}")
    message(FATAL_ERROR "Signal guardrail does not reject '${AO_LEGACY_SIGNAL_SPELLING}'")
  endif()
endforeach()

foreach(AO_ADJACENT_SIGNAL_SPELLING IN ITEMS
    "rt::SignalRouter"
    "rt::SubscriptionScope"
    "Chart::Signal"
    "Report::Subscription"
    "other::rt::Signal"
    "other::ao::rt::Signal")
  if("${AO_ADJACENT_SIGNAL_SPELLING}" MATCHES "${AO_FORBIDDEN_APPLICATION_SIGNAL_PRIMITIVE_REGEX}")
    message(FATAL_ERROR "Signal guardrail rejects adjacent vocabulary '${AO_ADJACENT_SIGNAL_SPELLING}'")
  endif()
endforeach()

foreach(AO_INTERNAL_PLAYBACK_SPELLING IN ITEMS
    "PlaybackTransport"
    "ao::rt::PlaybackSuccession"
    "runtime/playback/PlaybackTransport.h")
  if(NOT "${AO_INTERNAL_PLAYBACK_SPELLING}" MATCHES "${AO_FORBIDDEN_INTERNAL_PLAYBACK_REGEX}")
    message(FATAL_ERROR "Playback boundary guardrail does not reject '${AO_INTERNAL_PLAYBACK_SPELLING}'")
  endif()
endforeach()

foreach(AO_PUBLIC_PLAYBACK_SPELLING IN ITEMS
    "PlaybackTransportSnapshot"
    "PlaybackSuccessionSnapshot")
  if("${AO_PUBLIC_PLAYBACK_SPELLING}" MATCHES "${AO_FORBIDDEN_INTERNAL_PLAYBACK_REGEX}")
    message(FATAL_ERROR "Playback boundary guardrail rejects public value '${AO_PUBLIC_PLAYBACK_SPELLING}'")
  endif()
endforeach()
set(AO_FORBIDDEN_PLATFORM_UI_INCLUDE_REGEX
  "#[ \t]*include[ \t]*[<\\\"](gtkmm|gdkmm|giomm|glibmm|gtk|gdk|gio|glib)/")
set(AO_FORBIDDEN_UIMODEL_PLATFORM_VOCABULARY_REGEX
  "\"(ao-activity-status[A-Za-z0-9_.-]*|[A-Za-z0-9_.-]+-symbolic)\"")
set(AO_FORBIDDEN_UIMODEL_PLATFORM_REGEX
  "(${AO_FORBIDDEN_PLATFORM_UI_INCLUDE_REGEX})|(${AO_FORBIDDEN_UIMODEL_PLATFORM_VOCABULARY_REGEX})")
set(AO_FORBIDDEN_LOWER_PRESENTATION_VOCABULARY_REGEX
  "\"(ao-activity-status[A-Za-z0-9_.-]*|[A-Za-z0-9_.-]+-symbolic|Unknown (Artist|Album|Year|Genre|Composer|Conductor|Ensemble|Work)|Modern Linux audio server with low latency|Advanced Linux Sound Architecture [(]Direct Hardware Access[)]|Windows Audio Session API|WASAPI render endpoint|Shared Mode|Exclusive Mode|logical operator)\"")
set(AO_FORBIDDEN_TRACK_FIELD_COPY_REGEX "std::string_view[ \t]+label[ \t]*[;{]")
set(AO_FORBIDDEN_TRACK_PRESET_COPY_REGEX "std::string_view[ \t]+(label|description)[ \t]*[;{]")
set(AO_FORBIDDEN_BACKEND_DESCRIPTOR_COPY_REGEX "std::string[ \t]+(name|description|iconName)[ \t]*[;{]")
set(AO_FORBIDDEN_COMPLETION_DETAIL_COPY_REGEX "std::string[ \t]+detail[ \t]*[;{]")
set(AO_FORBIDDEN_CLI_LOCALIZATION_REGEX
  "#[ \t]*include[ \t]*[<\"]ao/i18n/")
set(AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX "(#[ \t]*include[ \t]*[<\\\"](runtime/|ao/(lmdb/|library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)|audio/(Player|Backend\\.h|BackendProvider\\.h|Engine|NullBackend|backend/|detail/))))|${AO_FORBIDDEN_WRITE_AUTHORITY_REGEX}")
set(AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX "(#[ \t]*include[ \t]*[<\\\"](ao/(lmdb/|library/)|${AO_FORBIDDEN_AUDIO_CONTROL_INCLUDE_REGEX}))|${AO_FORBIDDEN_WRITE_AUTHORITY_REGEX}")
# Reject every commands() accessor in a frontend after removing calls whose
# receiver is explicitly Playback-shaped. This also catches a Library hidden
# behind an auto alias, while preserving PlaybackService::commands().
set(AO_FORBIDDEN_FRONTEND_LIBRARY_COMMANDS_REGEX
  "(\\.|->)[ \t\r\n]*commands[ \t\r\n]*\\(")
set(AO_ALLOWED_FRONTEND_PLAYBACK_COMMANDS_REGEX
  "[A-Za-z0-9_]*[Pp]layback[A-Za-z0-9_]*[ \t\r\n]*(\\([ \t\r\n]*\\))?[ \t\r\n]*(\\.|->)[ \t\r\n]*commands[ \t\r\n]*\\(")
foreach(AO_FRONTEND_LIBRARY_COMMANDS_SPELLING IN ITEMS
    "runtime.library().commands()"
    "_library.commands()"
    "libraryPtr->commands()"
    "lib.commands()")
  set(AO_FILTERED_FRONTEND_COMMANDS_SPELLING "${AO_FRONTEND_LIBRARY_COMMANDS_SPELLING}")
  string(REGEX REPLACE
    "${AO_ALLOWED_FRONTEND_PLAYBACK_COMMANDS_REGEX}"
    ""
    AO_FILTERED_FRONTEND_COMMANDS_SPELLING
    "${AO_FILTERED_FRONTEND_COMMANDS_SPELLING}"
  )
  if(NOT "${AO_FILTERED_FRONTEND_COMMANDS_SPELLING}" MATCHES "${AO_FORBIDDEN_FRONTEND_LIBRARY_COMMANDS_REGEX}")
    message(FATAL_ERROR "Library command guardrail does not reject '${AO_FRONTEND_LIBRARY_COMMANDS_SPELLING}'")
  endif()
endforeach()
foreach(AO_FRONTEND_PLAYBACK_COMMANDS_SPELLING IN ITEMS
    "runtime.playback().commands()"
    "_playback.commands()"
    "playbackPtr->commands()")
  set(AO_FILTERED_FRONTEND_COMMANDS_SPELLING "${AO_FRONTEND_PLAYBACK_COMMANDS_SPELLING}")
  string(REGEX REPLACE
    "${AO_ALLOWED_FRONTEND_PLAYBACK_COMMANDS_REGEX}"
    ""
    AO_FILTERED_FRONTEND_COMMANDS_SPELLING
    "${AO_FILTERED_FRONTEND_COMMANDS_SPELLING}"
  )
  if("${AO_FILTERED_FRONTEND_COMMANDS_SPELLING}" MATCHES "${AO_FORBIDDEN_FRONTEND_LIBRARY_COMMANDS_REGEX}")
    message(FATAL_ERROR "Library command guardrail rejects '${AO_FRONTEND_PLAYBACK_COMMANDS_SPELLING}'")
  endif()
endforeach()
set(AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX "(#[ \t]*include[ \t]*[<\\\"](ao/rt/CoreRuntime[.]h|ao/lmdb/|ao/library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)))|${AO_FORBIDDEN_WRITE_AUTHORITY_REGEX}|(^|[^A-Za-z0-9_])LibraryCommands([^A-Za-z0-9_]|$)|${AO_FORBIDDEN_FRONTEND_LIBRARY_COMMANDS_REGEX}")
foreach(AO_FRONTEND_CORE_OWNER_SPELLING IN ITEMS
    "#include <ao/rt/CoreRuntime.h>"
    "#include \"ao/rt/CoreRuntime.h\"")
  if(NOT "${AO_FRONTEND_CORE_OWNER_SPELLING}" MATCHES "${AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX}")
    message(FATAL_ERROR "Frontend core boundary does not reject '${AO_FRONTEND_CORE_OWNER_SPELLING}'")
  endif()
endforeach()
if("#include <ao/rt/AppRuntime.h>" MATCHES "${AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX}")
  message(FATAL_ERROR "Frontend core boundary rejects AppRuntime")
endif()
set(AO_FORBIDDEN_MANAGED_STATE_MECHANISM_REGEX
  "ao/yaml/Reflect[.]h|namespace[ \t\r\n]+ao::yaml")
set(AO_FORBIDDEN_CORE_YAML_DOMAIN_REGEX
  "#[ \t]*include[ \t]*[<\\\"]ao/(CoreIds[.]h|rt/|uimodel/)|APP_LOG_|#[ \t]*include[ \t]*[<\\\"]spdlog/")

ao_write_guardrail_regex(AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX_FILE "rt-public" "${AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX}")
ao_write_guardrail_regex(
  AO_RUNTIME_WRITE_TRANSACTION_REGEX_FILE
  "runtime-write-transaction"
  "${AO_RUNTIME_WRITE_TRANSACTION_REGEX}"
)
ao_write_guardrail_regex(
  AO_RUNTIME_WRITE_TRANSACTION_OWNER_REGEX_FILE
  "runtime-write-transaction-owners"
  "${AO_RUNTIME_WRITE_TRANSACTION_OWNER_REGEX}"
)
ao_write_guardrail_regex(AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX_FILE "uimodel-core" "${AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX}")
ao_write_guardrail_regex(
  AO_FORBIDDEN_UIMODEL_PLATFORM_REGEX_FILE
  "uimodel-platform"
  "${AO_FORBIDDEN_UIMODEL_PLATFORM_REGEX}"
)
ao_write_guardrail_regex(AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX_FILE "frontend-core" "${AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX}")
ao_write_guardrail_regex(
  AO_ALLOWED_FRONTEND_PLAYBACK_COMMANDS_REGEX_FILE
  "frontend-playback-commands"
  "${AO_ALLOWED_FRONTEND_PLAYBACK_COMMANDS_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_FRONTEND_LIBRARY_PATH_REGEX_FILE
  "frontend-library-path"
  "${AO_FORBIDDEN_FRONTEND_LIBRARY_PATH_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_APPLICATION_SIGNAL_PRIMITIVE_REGEX_FILE
  "application-signal-primitive"
  "${AO_FORBIDDEN_APPLICATION_SIGNAL_PRIMITIVE_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_INTERNAL_PLAYBACK_REGEX_FILE
  "internal-playback"
  "${AO_FORBIDDEN_INTERNAL_PLAYBACK_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_MANAGED_STATE_MECHANISM_REGEX_FILE
  "managed-state-mechanism"
  "${AO_FORBIDDEN_MANAGED_STATE_MECHANISM_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_CORE_YAML_DOMAIN_REGEX_FILE
  "core-yaml-domain"
  "${AO_FORBIDDEN_CORE_YAML_DOMAIN_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_LOWER_PRESENTATION_VOCABULARY_REGEX_FILE
  "lower-presentation-vocabulary"
  "${AO_FORBIDDEN_LOWER_PRESENTATION_VOCABULARY_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_TRACK_FIELD_COPY_REGEX_FILE
  "track-field-copy"
  "${AO_FORBIDDEN_TRACK_FIELD_COPY_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_TRACK_PRESET_COPY_REGEX_FILE
  "track-preset-copy"
  "${AO_FORBIDDEN_TRACK_PRESET_COPY_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_BACKEND_DESCRIPTOR_COPY_REGEX_FILE
  "backend-descriptor-copy"
  "${AO_FORBIDDEN_BACKEND_DESCRIPTOR_COPY_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_COMPLETION_DETAIL_COPY_REGEX_FILE
  "completion-detail-copy"
  "${AO_FORBIDDEN_COMPLETION_DETAIL_COPY_REGEX}"
)
ao_write_guardrail_regex(
  AO_FORBIDDEN_CLI_LOCALIZATION_REGEX_FILE
  "cli-localization"
  "${AO_FORBIDDEN_CLI_LOCALIZATION_REGEX}"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/ao/yaml/ConfigTraits.h")
  message(FATAL_ERROR "Managed state must use owner-local explicit schemas; remove ConfigTraits.h")
endif()

foreach(AO_LEGACY_SIGNAL_HEADER IN ITEMS Signal.h Subscription.h)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/ao/rt/${AO_LEGACY_SIGNAL_HEADER}")
    message(FATAL_ERROR
      "Generic signal primitives belong in ao_async; remove app/include/ao/rt/${AO_LEGACY_SIGNAL_HEADER}")
  endif()
endforeach()

foreach(AO_LEGACY_PLAYBACK_HEADER IN ITEMS PlaybackService.h PlaybackSequenceService.h)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/ao/rt/${AO_LEGACY_PLAYBACK_HEADER}")
    message(FATAL_ERROR
      "Internal playback owners must not be public headers; remove app/include/ao/rt/${AO_LEGACY_PLAYBACK_HEADER}")
  endif()
endforeach()

add_custom_target(ao_shared_signal_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${CMAKE_SOURCE_DIR}/test"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_APPLICATION_SIGNAL_PRIMITIVE_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking that generic signal primitives stay below application namespaces"
  VERBATIM
)

add_custom_target(ao_runtime_write_transaction_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/runtime"
          "-DFORBIDDEN_REGEX_FILE=${AO_RUNTIME_WRITE_TRANSACTION_REGEX_FILE}"
          "-DEXCLUDE_REGEX_FILE=${AO_RUNTIME_WRITE_TRANSACTION_OWNER_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking live runtime write transactions have one sequencer owner"
  VERBATIM
)

add_custom_target(ao_playback_internal_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/rt;${CMAKE_SOURCE_DIR}/app/include/ao/uimodel;${CMAKE_SOURCE_DIR}/app/include/ao/desktop;${CMAKE_SOURCE_DIR}/app/uimodel;${CMAKE_SOURCE_DIR}/app/desktop;${CMAKE_SOURCE_DIR}/app/linux-gtk;${CMAKE_SOURCE_DIR}/app/windows-winui;${CMAKE_SOURCE_DIR}/app/tui;${CMAKE_SOURCE_DIR}/app/cli"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_INTERNAL_PLAYBACK_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking internal playback owners do not leak into public or consumer layers"
  VERBATIM
)

add_custom_target(ao_presentation_vocabulary_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/include;${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/app/include/ao/rt;${CMAKE_SOURCE_DIR}/app/runtime"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_LOWER_PRESENTATION_VOCABULARY_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/rt/TrackField.h"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_TRACK_FIELD_COPY_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/rt/TrackPresentation.h"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_TRACK_PRESET_COPY_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/include/ao/audio/BackendProvider.h"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_BACKEND_DESCRIPTOR_COPY_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/rt/completion/CompletionItem.h"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_COMPLETION_DETAIL_COPY_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking shared presentation copy stays out of Core and runtime"
  VERBATIM
)

add_custom_target(ao_managed_state_schema_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include;${CMAKE_SOURCE_DIR}/app/runtime;${CMAKE_SOURCE_DIR}/app/uimodel;${CMAKE_SOURCE_DIR}/app/desktop;${CMAKE_SOURCE_DIR}/app/linux-gtk;${CMAKE_SOURCE_DIR}/app/windows-winui"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_MANAGED_STATE_MECHANISM_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking application-managed state uses owner-local explicit schemas"
  VERBATIM
)

add_custom_target(ao_core_yaml_domain_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/include/ao/yaml"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_CORE_YAML_DOMAIN_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking core YAML helpers remain application-domain neutral"
  VERBATIM
)

add_custom_target(ao_rt_public_boundary_report
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/rt"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking storage/control-plane includes that leak through ao/rt public headers"
  VERBATIM
)

add_custom_target(ao_uimodel_platform_boundary_report
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/uimodel"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_UIMODEL_PLATFORM_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/uimodel"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_UIMODEL_PLATFORM_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking platform UI includes and vocabulary in ao_app_uimodel"
  VERBATIM
)

add_custom_target(ao_uimodel_core_boundary_report
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/uimodel"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/uimodel"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking storage/control-plane includes in ao_app_uimodel"
  VERBATIM
)

if(AOBUS_BUILD_GTK)
  add_custom_target(ao_linux_gtk_core_boundary_report
    COMMAND ${CMAKE_COMMAND}
            "-DROOTS=${CMAKE_SOURCE_DIR}/app/linux-gtk"
            "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX_FILE}"
            "-DALLOWED_REGEX_FILE=${AO_ALLOWED_FRONTEND_PLAYBACK_COMMANDS_REGEX_FILE}"
            -DMODE=enforce
            -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
    COMMENT "Checking direct storage/library includes in linux-gtk"
    VERBATIM
  )
endif()

if(AOBUS_BUILD_TUI)
  add_custom_target(ao_tui_core_boundary_report
    COMMAND ${CMAKE_COMMAND}
            "-DROOTS=${CMAKE_SOURCE_DIR}/app/tui"
            "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX_FILE}"
            "-DALLOWED_REGEX_FILE=${AO_ALLOWED_FRONTEND_PLAYBACK_COMMANDS_REGEX_FILE}"
            -DMODE=enforce
            -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
    COMMENT "Checking direct storage/library dependencies in the TUI"
    VERBATIM
  )
endif()

add_custom_target(ao_windows_winui_core_boundary_report
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/windows-winui"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX_FILE}"
          "-DALLOWED_REGEX_FILE=${AO_ALLOWED_FRONTEND_PLAYBACK_COMMANDS_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking direct storage/library dependencies in the WinUI frontend"
  VERBATIM
)

add_custom_target(ao_app_desktop_core_boundary_report
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/desktop;${CMAKE_SOURCE_DIR}/app/desktop"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX_FILE}"
          "-DALLOWED_REGEX_FILE=${AO_ALLOWED_FRONTEND_PLAYBACK_COMMANDS_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking direct storage/library dependencies in ao_app_desktop"
  VERBATIM
)

# Hand-revoked WinRT events are what grew the frontend's teardown catches, and
# no lint the project runs can see them: a comment in the catch satisfies
# bugprone-empty-catch.
add_custom_target(ao_windows_winui_event_revoker_guardrail
  COMMAND ${CMAKE_COMMAND}
          "-DROOT=${CMAKE_SOURCE_DIR}/app/windows-winui"
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertWinUiEventRevokers.cmake
  COMMENT "Checking WinUI revokes WinRT events by destructor rather than by hand"
  VERBATIM
)

# UI state delivery is source-owned and component-scoped. Keep the removed
# virtual callback/raw observer graph from returning in the native WinUI target.
add_custom_target(ao_windows_winui_state_subscription_guardrail
  COMMAND ${CMAKE_COMMAND}
          "-DROOT=${CMAKE_SOURCE_DIR}/app/windows-winui"
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertWinUiStateSubscriptions.cmake
  COMMENT "Checking WinUI state delivery remains subscription-owned"
  VERBATIM
)

# Generation components and leaf adapters bind exact capabilities. Session and
# runtime composition remain at MainWindow and ShellBuilder.
add_custom_target(ao_windows_winui_leaf_capability_guardrail
  COMMAND ${CMAKE_COMMAND}
          "-DROOT=${CMAKE_SOURCE_DIR}/app/windows-winui"
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertWinUiLeafCapabilities.cmake
  COMMENT "Checking WinUI leaves receive narrow capabilities"
  VERBATIM
)

add_custom_target(ao_frontend_library_path_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/desktop;${CMAKE_SOURCE_DIR}/app/desktop;${CMAKE_SOURCE_DIR}/app/linux-gtk;${CMAKE_SOURCE_DIR}/app/windows-winui;${CMAKE_SOURCE_DIR}/app/tui;${CMAKE_SOURCE_DIR}/app/cli"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_FRONTEND_LIBRARY_PATH_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking frontends do not duplicate canonical per-library paths"
  VERBATIM
)

add_custom_target(ao_layering_boundary_report
  DEPENDS
    ao_app_desktop_core_boundary_report
    ao_core_yaml_domain_boundary_check
    ao_frontend_library_path_boundary_check
    ao_managed_state_schema_boundary_check
    ao_playback_internal_boundary_check
    ao_presentation_vocabulary_boundary_check
    ao_rt_public_boundary_report
    ao_shared_signal_boundary_check
    ao_uimodel_platform_boundary_report
    ao_uimodel_core_boundary_report
    ao_uimodel_frontend_vocabulary_guardrail
)

if(AOBUS_BUILD_GTK)
  add_dependencies(ao_layering_boundary_report ao_linux_gtk_core_boundary_report)
endif()
if(AOBUS_BUILD_TUI)
  add_dependencies(ao_layering_boundary_report ao_tui_core_boundary_report)
endif()
add_dependencies(ao_layering_boundary_report ao_windows_winui_core_boundary_report)

# Configure-time check: ao_app_uimodel must not link platform UI targets
get_target_property(_ao_uimodel_links ao_app_uimodel LINK_LIBRARIES)
foreach(_lib IN LISTS _ao_uimodel_links)
  if(_lib MATCHES "PkgGTKMM|gtkmm|gdkmm|giomm|glibmm|WinUI|WindowsApp")
    message(FATAL_ERROR
      "ao_app_uimodel must not link platform UI libraries (found: ${_lib})")
  endif()
endforeach()

# Frontends
if(AOBUS_BUILD_GTK)
  add_subdirectory(linux-gtk)
endif()
if(AOBUS_BUILD_CLI)
  add_subdirectory(cli)

  add_custom_target(ao_cli_localization_boundary_check
    COMMAND ${CMAKE_COMMAND}
            "-DROOTS=${CMAKE_SOURCE_DIR}/app/cli"
            "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_CLI_LOCALIZATION_REGEX_FILE}"
            -DMODE=enforce
            -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
    COMMENT "Checking that CLI source does not consume interactive localization"
    VERBATIM)

  function(aobus_assert_cli_link_closure root_target)
    set(_queue "${root_target}")
    set(_visited)
    while(_queue)
      list(POP_FRONT _queue _target)
      if(_target IN_LIST _visited OR NOT TARGET "${_target}")
        continue()
      endif()
      list(APPEND _visited "${_target}")

      foreach(_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_links "${_target}" "${_property}")
        if(NOT _links OR _links STREQUAL "_links-NOTFOUND")
          continue()
        endif()
        foreach(_link IN LISTS _links)
          if(_link MATCHES "ao_app_i18n(_ordering)?|ao_catalog_pattern|ICU::i18n")
            message(FATAL_ERROR
              "CLI target '${root_target}' reaches forbidden localization dependency "
              "'${_link}' through '${_target}'.")
          endif()
          string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_.:+-]*" _link_tokens "${_link}")
          foreach(_token IN LISTS _link_tokens)
            if(TARGET "${_token}" AND NOT _token IN_LIST _visited)
              list(APPEND _queue "${_token}")
            endif()
          endforeach()
        endforeach()
      endforeach()
    endwhile()
  endfunction()

  aobus_assert_cli_link_closure(aobus)
endif()
if(AOBUS_BUILD_TUI)
  add_subdirectory(tui)
endif()
if(AOBUS_BUILD_WINUI)
  add_subdirectory(windows-winui)
endif()
