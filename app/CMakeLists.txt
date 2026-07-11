# app/runtime: Shared application runtime (UI-neutral, used by all frontends)
add_library(ao_app_runtime STATIC
    runtime/AppRuntime.cpp
    runtime/CoreRuntime.cpp
    runtime/library/Library.cpp
    runtime/library/LibraryReader.cpp
    runtime/library/LibraryChanges.cpp
    runtime/library/LibraryYamlImporter.cpp
    runtime/library/LibraryYamlExporter.cpp
    runtime/completion/CompletionService.cpp
    runtime/completion/QueryExpressionCompleter.cpp
    runtime/completion/MetadataValueCompleter.cpp
  runtime/source/AllTracksSource.cpp
  runtime/source/IndexedTrackSequence.cpp
    runtime/source/CachedListSource.cpp
    runtime/ConfigStore.cpp
    runtime/PlaybackService.cpp
    runtime/PlaybackSequenceService.cpp
    runtime/PlaybackSessionPersistence.cpp
    runtime/playback/PlaybackCursor.cpp
    runtime/playback/PlaybackCursorSession.cpp
    runtime/playback/PlaybackRestartDeadline.cpp
    runtime/playback/PreparedNextRegistry.cpp
    runtime/playback/ProjectionAnchor.cpp
    runtime/playback/ShuffleHistory.cpp
    runtime/library/LibraryWriter.cpp
    runtime/library/AudioIdentityIndexer.cpp
    runtime/library/LibraryScan.cpp
    runtime/library/ScanPlanBuilder.cpp
    runtime/library/ScanApplyOperation.cpp
    runtime/library/LibraryTaskService.cpp
    runtime/source/TrackSourceCache.cpp
    runtime/source/ManualListSource.cpp
    runtime/NavigationHistory.cpp
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
    uimodel/layout/action/LayoutActionCatalog.cpp
    uimodel/layout/action/LayoutActionValidator.cpp
    uimodel/layout/component/AbsoluteCanvasGeometry.cpp
    uimodel/layout/component/LayoutComponentCatalog.cpp
    uimodel/layout/component/LayoutComponentState.cpp
    uimodel/layout/component/LayoutStatePromoter.cpp
    uimodel/layout/document/LayoutDocument.cpp
    uimodel/layout/document/LayoutNodeId.cpp
    uimodel/layout/document/LayoutTemplateExpansion.cpp
    uimodel/layout/document/LayoutValue.cpp
    uimodel/layout/shell/ShellLayoutSessionModel.cpp
    uimodel/input/KeyChord.cpp
    uimodel/input/KeymapModel.cpp
    uimodel/input/KeymapStore.cpp
    uimodel/preferences/PreferencesEditorModel.cpp
    uimodel/field/TrackFieldEditCodec.cpp
    uimodel/field/TrackFieldEditPolicy.cpp
    uimodel/field/TrackFieldFormatter.cpp
    uimodel/field/TrackInlineEdit.cpp
    uimodel/library/detail/TrackCustomMetadataWorkflow.cpp
    uimodel/library/detail/TrackFieldGridSchema.cpp
    uimodel/library/list/ListActionPolicy.cpp
    uimodel/library/list/ListTreeProjection.cpp
    uimodel/library/list/SmartListDraft.cpp
    uimodel/library/list/SmartListEditorViewState.cpp
    uimodel/library/list/SmartListExpression.cpp
    uimodel/library/list/SmartListPreview.cpp
    uimodel/library/list/SmartListTrackPresentationResolver.cpp
    uimodel/library/presentation/TrackColumnLayoutPolicy.cpp
    uimodel/library/presentation/TrackColumnLayoutStore.cpp
    uimodel/library/presentation/TrackColumnWidthSolver.cpp
    uimodel/library/presentation/CustomPresentationEditorModel.cpp
    uimodel/library/presentation/TrackFieldPresentationPolicy.cpp
    uimodel/library/presentation/TrackPresentationCatalog.cpp
    uimodel/library/presentation/ListPresentationPreferenceStore.cpp
    uimodel/library/presentation/TrackPresentationRecommender.cpp
    uimodel/library/presentation/TrackPresentationPickerViewModel.cpp
    uimodel/library/property/TagEditWorkflow.cpp
    uimodel/library/property/TrackPropertiesFormSpec.cpp
    uimodel/library/property/TrackPropertiesFormModel.cpp
    uimodel/library/track/TrackSelectionSummary.cpp
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
set(AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX "#[ \t]*include[ \t]*[<\\\"](runtime/|ao/(lmdb/|library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)|audio/(Player|Backend\\.h|BackendProvider\\.h|Engine|NullBackend|backend/|detail/)))")
set(AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX "#[ \t]*include[ \t]*[<\\\"](ao/lmdb/|ao/library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)|${AO_FORBIDDEN_AUDIO_CONTROL_INCLUDE_REGEX})")
set(AO_FORBIDDEN_GTK_CORE_INCLUDE_REGEX "#[ \t]*include[ \t]*[<\\\"](ao/lmdb/|ao/library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView))")
set(AO_LINUX_GTK_BOUNDARY_REPORT_EXCLUDE_REGEX [=[/platform/|/MainWindowCoordinator\.cpp|/AudioBackendBootstrap\.]=])

set(AO_GUARDRAIL_REGEX_DIR "${CMAKE_CURRENT_BINARY_DIR}/guardrail-regex")
file(MAKE_DIRECTORY "${AO_GUARDRAIL_REGEX_DIR}")
function(ao_write_guardrail_regex out_var name pattern)
  set(_path "${AO_GUARDRAIL_REGEX_DIR}/${name}.regex")
  file(WRITE "${_path}" "${pattern}")
  set(${out_var} "${_path}" PARENT_SCOPE)
endfunction()

ao_write_guardrail_regex(AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX_FILE "rt-public" "${AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX}")
ao_write_guardrail_regex(AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX_FILE "uimodel-core" "${AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX}")
ao_write_guardrail_regex(AO_FORBIDDEN_GTK_CORE_INCLUDE_REGEX_FILE "gtk-core" "${AO_FORBIDDEN_GTK_CORE_INCLUDE_REGEX}")
ao_write_guardrail_regex(AO_LINUX_GTK_BOUNDARY_REPORT_EXCLUDE_REGEX_FILE "linux-gtk-exclude" "${AO_LINUX_GTK_BOUNDARY_REPORT_EXCLUDE_REGEX}")

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
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/uimodel"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking platform UI includes in ao_app_uimodel"
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
            "-DFORBIDDEN_REGEX_FILE=${AO_FORBIDDEN_GTK_CORE_INCLUDE_REGEX_FILE}"
            "-DEXCLUDE_REGEX_FILE=${AO_LINUX_GTK_BOUNDARY_REPORT_EXCLUDE_REGEX_FILE}"
            -DMODE=enforce
            -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
    COMMENT "Checking direct storage/library includes in linux-gtk"
    VERBATIM
  )
endif()

add_custom_target(ao_layering_boundary_report
  DEPENDS
    ao_rt_public_boundary_report
    ao_uimodel_platform_boundary_report
    ao_uimodel_core_boundary_report
)
if(AOBUS_BUILD_GTK)
  add_dependencies(ao_layering_boundary_report ao_linux_gtk_core_boundary_report)
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

# Placeholders for future platforms
# add_subdirectory(windows-winui)
