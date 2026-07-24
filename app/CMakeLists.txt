# app/runtime: Shared application runtime (UI-neutral, used by all frontends)
add_library(ao_app_runtime STATIC
    runtime/AppRuntime.cpp
    runtime/CoreRuntime.cpp
    runtime/library/Library.cpp
    runtime/library/LibraryPaths.cpp
    runtime/library/LibraryMutationService.cpp
    runtime/library/LibraryReader.cpp
    runtime/library/LibraryChanges.cpp
    runtime/library/MediaTrack.cpp
    runtime/library/TrackBuilderSnapshot.cpp
    runtime/library/LibraryYamlImporter.cpp
    runtime/library/LibraryYamlExporter.cpp
    runtime/completion/CompletionService.cpp
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
    runtime/playback/PlaybackService.cpp
    runtime/playback/PlaybackCursor.cpp
    runtime/playback/PlaybackCursorSession.cpp
    runtime/playback/PlaybackRestartDeadline.cpp
    runtime/playback/PreparedNextRegistry.cpp
    runtime/playback/ProjectionAnchor.cpp
    runtime/playback/ShuffleHistory.cpp
    runtime/library/LibraryWriter.cpp
    runtime/library/AudioIdentityBatchWriter.cpp
    runtime/library/AudioIdentityIndexer.cpp
    runtime/library/LibraryScan.cpp
    runtime/library/ScanPlanBuilder.cpp
    runtime/library/ScanPlan.cpp
    runtime/library/ScanApplyOperation.cpp
    runtime/library/LibraryTaskService.cpp
    runtime/source/TrackSourceCache.cpp
    runtime/source/ManualListSource.cpp
    runtime/NavigationHistory.cpp
    runtime/WorkspaceSessionYamlSchema.cpp
    runtime/WorkspaceService.cpp
    runtime/NotificationService.cpp
    runtime/source/SmartListEvaluator.cpp
    runtime/source/SmartListSource.cpp
    runtime/TrackPresentation.cpp
    runtime/TrackField.cpp
    runtime/TrackFieldReader.cpp
    runtime/TrackEditScript.cpp
    runtime/projection/LiveTrackListProjection.cpp
    runtime/projection/TrackProjectionEditScript.cpp
    runtime/projection/LiveTrackDetailProjection.cpp
    runtime/source/TrackSource.cpp
    runtime/source/TrackSourceDeltaBuilder.cpp
    runtime/source/TrackSourceEditScript.cpp
    runtime/ViewService.cpp
    runtime/Log.cpp
)
target_include_directories(ao_app_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ao_app_runtime PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_runtime PUBLIC ao ao_async PkgRapidYaml spdlog::spdlog)
target_link_libraries(ao_app_runtime PRIVATE Boost::headers)

# ao_app_uimodel: platform-neutral UI models (reusable by linux-gtk, WinUI 3, tests)
add_library(ao_app_uimodel STATIC
    uimodel/presentation/PresentationTextCatalog.cpp
    uimodel/layout/action/LayoutActionCatalog.cpp
    uimodel/layout/action/LayoutActionValidator.cpp
    uimodel/layout/component/AbsoluteCanvasGeometry.cpp
    uimodel/layout/component/LayoutComponentCatalog.cpp
    uimodel/layout/component/LayoutComponentState.cpp
    uimodel/layout/component/LayoutStatePromoter.cpp
    uimodel/layout/document/LayoutDocument.cpp
    uimodel/layout/document/LayoutNodeId.cpp
    uimodel/layout/document/LayoutPreparation.cpp
    uimodel/layout/document/LayoutValue.cpp
    uimodel/layout/shell/ShellLayoutSessionModel.cpp
    uimodel/input/KeyChord.cpp
    uimodel/input/KeymapModel.cpp
    uimodel/input/KeymapStore.cpp
    uimodel/preference/PreferencesEditorModel.cpp
    uimodel/field/TrackFieldEditCodec.cpp
    uimodel/field/TrackFieldEditPolicy.cpp
    uimodel/field/TrackFieldFormatter.cpp
    uimodel/library/detail/TrackCustomMetadata.cpp
    uimodel/library/detail/TrackFieldGridSchema.cpp
    uimodel/library/list/ListActionPolicy.cpp
    uimodel/library/list/ListTreeProjection.cpp
    uimodel/library/list/SmartListDraft.cpp
    uimodel/library/list/SmartListEditorViewState.cpp
    uimodel/library/list/SmartListExpression.cpp
    uimodel/library/list/SmartListPreview.cpp
    uimodel/library/list/SmartListTrackPresentationResolver.cpp
    uimodel/library/presentation/TrackColumnLayoutPolicy.cpp
    uimodel/library/presentation/TrackColumnLayoutYamlSchema.cpp
    uimodel/library/presentation/TrackColumnLayoutStore.cpp
    uimodel/library/presentation/TrackColumnWidthSolver.cpp
    uimodel/library/presentation/CustomPresentationEditorModel.cpp
    uimodel/library/presentation/TrackGroupHeadingPresentation.cpp
    uimodel/library/presentation/TrackFieldPresentationPolicy.cpp
    uimodel/library/presentation/TrackPresentationCatalog.cpp
    uimodel/library/presentation/ListPresentationPreferenceYamlSchema.cpp
    uimodel/library/presentation/ListPresentationPreferenceStore.cpp
    uimodel/library/presentation/TrackPresentationRecommender.cpp
    uimodel/library/presentation/TrackPresentationPickerViewModel.cpp
    uimodel/library/property/TagEdit.cpp
    uimodel/library/property/TrackAuthoringSession.cpp
    uimodel/library/property/TrackPropertiesFormSpec.cpp
    uimodel/library/property/TrackPropertiesFormModel.cpp
    uimodel/library/track/TrackCountFormatter.cpp
    uimodel/library/track/TrackSelectionSummary.cpp
    uimodel/library/track/TrackFilterPolicy.cpp
    uimodel/library/track/TrackFilterCompleter.cpp
    uimodel/library/track/TrackFilterResolver.cpp
    uimodel/library/track/TrackFilterViewModel.cpp
    uimodel/library/track/TrackPageRoute.cpp
    uimodel/playback/command/PlaybackCommandSurface.cpp
    uimodel/playback/now-playing/NowPlayingViewModel.cpp
    uimodel/playback/output/OutputDeviceViewModel.cpp
    uimodel/playback/output/VolumeViewModel.cpp
    uimodel/playback/quality/AudioQualityFormatter.cpp
    uimodel/playback/seek/PlaybackPositionInterpolator.cpp
    uimodel/playback/seek/PlaybackTimeViewModel.cpp
    uimodel/playback/seek/SeekSliderInteractionModel.cpp
    uimodel/playback/seek/SeekViewModel.cpp
    uimodel/playback/soul/AobusSoulViewModel.cpp
    uimodel/playback/transport/TransportViewModel.cpp
    uimodel/status/activity/ActivityStatusFeedProjection.cpp
    uimodel/status/activity/ActivityStatusViewModel.cpp
)
target_include_directories(ao_app_uimodel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ao_app_uimodel PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_uimodel PUBLIC ao_app_runtime)
target_link_libraries(ao_app_uimodel PRIVATE)

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

add_dependencies(ao_app_uimodel ao_uimodel_layout_boundary_check)

add_custom_target(ao_uimodel_organization_guardrail
  COMMAND ${CMAKE_COMMAND}
          "-DPUBLIC_ROOT=${CMAKE_SOURCE_DIR}/app/include/ao/uimodel"
          "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}/app/uimodel"
          "-DTEST_ROOT=${CMAKE_SOURCE_DIR}/test/unit/uimodel"
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertUimodelOrganization.cmake
  COMMENT "Checking UIModel namespace and folder organization"
  VERBATIM
)

add_dependencies(ao_app_uimodel ao_uimodel_organization_guardrail)

# --- Layering boundary guardrails ---
# Cleared boundaries run in enforce mode and are attached to the owning target.
# ao_layering_boundary_report remains as an aggregate entry point.
set(AO_FORBIDDEN_AUDIO_CONTROL_INCLUDE_REGEX "ao/audio/(Player|Backend\\.h|BackendProvider\\.h|Engine|NullBackend|backend/|detail/)")
set(AO_FORBIDDEN_WRITE_AUTHORITY_REGEX "(^|[^A-Za-z0-9_])(WriteTransaction|WritableMusicLibrary)([^A-Za-z0-9_]|$)")
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
set(AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX "(#[ \t]*include[ \t]*[<\\\"](runtime/|ao/(lmdb/|library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)|audio/(Player|Backend\\.h|BackendProvider\\.h|Engine|NullBackend|backend/|detail/))))|${AO_FORBIDDEN_WRITE_AUTHORITY_REGEX}")
set(AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX "(#[ \t]*include[ \t]*[<\\\"](ao/(lmdb/|library/)|${AO_FORBIDDEN_AUDIO_CONTROL_INCLUDE_REGEX}))|${AO_FORBIDDEN_WRITE_AUTHORITY_REGEX}")
set(AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX "(#[ \t]*include[ \t]*[<\\\"](ao/lmdb/|ao/library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)))|${AO_FORBIDDEN_WRITE_AUTHORITY_REGEX}|(^|[^A-Za-z0-9_])LibraryWriter([^A-Za-z0-9_]|$)|\\.writer[ \t\r\n]*\\(")
set(AO_FORBIDDEN_MANAGED_STATE_MECHANISM_REGEX
  "ao/yaml/Reflect[.]h|namespace[ \t\r\n]+ao::yaml")
set(AO_FORBIDDEN_CORE_YAML_DOMAIN_REGEX
  "#[ \t]*include[ \t]*[<\\\"]ao/(CoreIds[.]h|rt/|uimodel/)|APP_LOG_|#[ \t]*include[ \t]*[<\\\"]spdlog/")

ao_write_guardrail_regex(AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX_FILE "rt-public" "${AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX}")
ao_write_guardrail_regex(AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX_FILE "uimodel-core" "${AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX}")
ao_write_guardrail_regex(
  AO_FORBIDDEN_UIMODEL_PLATFORM_REGEX_FILE
  "uimodel-platform"
  "${AO_FORBIDDEN_UIMODEL_PLATFORM_REGEX}"
)
ao_write_guardrail_regex(AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX_FILE "frontend-core" "${AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX}")
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

add_dependencies(ao_app_runtime ao_shared_signal_boundary_check)

add_custom_target(ao_playback_internal_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/rt;${CMAKE_SOURCE_DIR}/app/include/ao/uimodel;${CMAKE_SOURCE_DIR}/app/uimodel;${CMAKE_SOURCE_DIR}/app/linux-gtk;${CMAKE_SOURCE_DIR}/app/windows-winui;${CMAKE_SOURCE_DIR}/app/tui;${CMAKE_SOURCE_DIR}/app/cli"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_INTERNAL_PLAYBACK_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking internal playback owners do not leak into public or consumer layers"
  VERBATIM
)

add_dependencies(ao_app_runtime ao_playback_internal_boundary_check)

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

add_dependencies(ao_app_runtime ao_presentation_vocabulary_boundary_check)

add_custom_target(ao_managed_state_schema_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include;${CMAKE_SOURCE_DIR}/app/runtime;${CMAKE_SOURCE_DIR}/app/uimodel;${CMAKE_SOURCE_DIR}/app/linux-gtk"
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

add_dependencies(ao_app_runtime ao_managed_state_schema_boundary_check ao_core_yaml_domain_boundary_check)
add_dependencies(ao_app_uimodel ao_managed_state_schema_boundary_check)

add_custom_target(ao_rt_public_boundary_report
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/rt"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking storage/control-plane includes that leak through ao/rt public headers"
  VERBATIM
)

add_dependencies(ao_app_runtime ao_rt_public_boundary_report)

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

add_dependencies(ao_app_uimodel ao_uimodel_platform_boundary_report ao_uimodel_core_boundary_report)

if(AOBUS_BUILD_GTK)
  add_custom_target(ao_linux_gtk_core_boundary_report
    COMMAND ${CMAKE_COMMAND}
            "-DROOTS=${CMAKE_SOURCE_DIR}/app/linux-gtk"
            "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX_FILE}"
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
            -DMODE=enforce
            -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
    COMMENT "Checking direct storage/library dependencies in the TUI"
    VERBATIM
  )
endif()

if(AOBUS_BUILD_WINUI)
  add_custom_target(ao_windows_winui_core_boundary_report
    COMMAND ${CMAKE_COMMAND}
            "-DROOTS=${CMAKE_SOURCE_DIR}/app/windows-winui"
            "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_FRONTEND_CORE_INCLUDE_REGEX_FILE}"
            -DMODE=enforce
            -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
    COMMENT "Checking direct storage/library dependencies in the WinUI frontend"
    VERBATIM
  )
endif()

add_custom_target(ao_frontend_library_path_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/linux-gtk;${CMAKE_SOURCE_DIR}/app/windows-winui;${CMAKE_SOURCE_DIR}/app/tui;${CMAKE_SOURCE_DIR}/app/cli"
          "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_FRONTEND_LIBRARY_PATH_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking frontends do not duplicate canonical per-library paths"
  VERBATIM
)

add_custom_target(ao_layering_boundary_report
  DEPENDS
    ao_core_yaml_domain_boundary_check
    ao_frontend_library_path_boundary_check
    ao_managed_state_schema_boundary_check
    ao_playback_internal_boundary_check
    ao_presentation_vocabulary_boundary_check
    ao_rt_public_boundary_report
    ao_shared_signal_boundary_check
    ao_uimodel_platform_boundary_report
    ao_uimodel_core_boundary_report
)
if(AOBUS_BUILD_GTK)
  add_dependencies(ao_layering_boundary_report ao_linux_gtk_core_boundary_report)
endif()
if(AOBUS_BUILD_TUI)
  add_dependencies(ao_layering_boundary_report ao_tui_core_boundary_report)
endif()
if(AOBUS_BUILD_WINUI)
  add_dependencies(ao_layering_boundary_report ao_windows_winui_core_boundary_report)
endif()

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
endif()
if(AOBUS_BUILD_TUI)
  add_subdirectory(tui)
endif()
if(AOBUS_BUILD_WINUI)
  add_subdirectory(windows-winui)
  add_dependencies(aobus-winui
    ao_frontend_library_path_boundary_check
    ao_windows_winui_core_boundary_report)
endif()
