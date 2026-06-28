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
    runtime/ConfigStore.cpp
    runtime/PlaybackService.cpp
    runtime/library/LibraryWriter.cpp
    runtime/library/LibraryTasks.cpp
    runtime/source/ListSourceStore.cpp
    runtime/source/ManualListSource.cpp
    runtime/NavigationHistory.cpp
    runtime/WorkspaceService.cpp
    runtime/NotificationService.cpp
    runtime/source/SmartListEvaluator.cpp
    runtime/source/SmartListSource.cpp
    runtime/TrackPresentation.cpp
    runtime/TrackField.cpp
    runtime/TrackFieldReader.cpp
    runtime/projection/TrackListProjection.cpp
    runtime/projection/TrackDetailProjection.cpp
    runtime/source/TrackSource.cpp
    runtime/ViewService.cpp
    runtime/Log.cpp
)
target_include_directories(ao_app_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ao_app_runtime PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_runtime PUBLIC ao ao_async PkgRapidYaml spdlog::spdlog)
target_link_libraries(ao_app_runtime PRIVATE Boost::headers)

# ao_app_uimodel: platform-neutral UI models (reusable by linux-gtk, WinUI 3, tests)
add_library(ao_app_uimodel STATIC
    uimodel/layout/AbsoluteCanvasModel.cpp
    uimodel/layout/ActionCatalog.cpp
    uimodel/layout/ActionValidator.cpp
    uimodel/layout/ComponentCatalog.cpp
    uimodel/layout/LayoutComponentState.cpp
    uimodel/layout/LayoutDocument.cpp
    uimodel/layout/LayoutNodeId.cpp
    uimodel/layout/LayoutStatePromoter.cpp
    uimodel/layout/LayoutTemplateExpander.cpp
    uimodel/layout/LayoutValue.cpp
    uimodel/layout/ShellLayoutSessionModel.cpp
    uimodel/input/KeyChord.cpp
    uimodel/input/KeymapModel.cpp
    uimodel/input/KeymapStore.cpp
    uimodel/list/ListActionPolicy.cpp
    uimodel/list/ListTreeProjection.cpp
    uimodel/list/SmartListEditorModel.cpp
    uimodel/playback/AobusSoulViewModel.cpp
    uimodel/playback/AudioOutputViewModel.cpp
    uimodel/playback/AudioQualityFormat.cpp
    uimodel/playback/NowPlayingViewModel.cpp
    uimodel/playback/PlaybackPositionInterpolator.cpp
    uimodel/playback/PlaybackQueueModel.cpp
    uimodel/playback/PlaybackTimeViewModel.cpp
    uimodel/playback/SeekSliderInteractionModel.cpp
    uimodel/playback/SeekViewModel.cpp
    uimodel/playback/TransportViewModel.cpp
    uimodel/playback/VolumeViewModel.cpp
    uimodel/status/ActivityStatusModel.cpp
    uimodel/track/TrackCustomPropertyWorkflow.cpp
    uimodel/track/TrackCustomViewEditorModel.cpp
    uimodel/track/TrackPageRoute.cpp
    uimodel/track/TrackFilterViewModel.cpp
    uimodel/track/TrackFilterResolver.cpp
    uimodel/track/TrackInlineEditWorkflow.cpp
    uimodel/track/TrackFieldEditPolicy.cpp
    uimodel/track/TrackFieldGridProjection.cpp
    uimodel/track/TrackFieldPresentationPolicy.cpp
    uimodel/track/TrackColumnLayoutPolicy.cpp
    uimodel/track/TrackColumnLayoutStore.cpp
    uimodel/track/TrackPresentationCatalog.cpp
    uimodel/track/TrackPresentationPreferenceStore.cpp
    uimodel/track/TrackPresentationRecommender.cpp
    uimodel/track/TrackPresentationWorkflow.cpp
    uimodel/track/TrackFieldFormatter.cpp
    uimodel/track/SelectionSummary.cpp
    uimodel/tag/TagEditWorkflow.cpp
    uimodel/tag/TrackPropertiesFormSpec.cpp
    uimodel/tag/TrackPropertiesFormWorkflow.cpp
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

# --- Layering boundary guardrails ---
# Cleared boundaries run in enforce mode and are attached to the owning target.
# ao_layering_boundary_report remains as an aggregate entry point.
set(AO_FORBIDDEN_AUDIO_CONTROL_INCLUDE_REGEX "ao/audio/(Player|IBackend|IBackendProvider|Engine|NullBackend|backend/|detail/)")
set(AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX "#[ \t]*include[ \t]*[<\\\"]ao/(lmdb/|library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)|audio/(Player|IBackend|IBackendProvider|Engine|NullBackend|backend/|detail/))")
set(AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX "#[ \t]*include[ \t]*[<\\\"](ao/lmdb/|ao/library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)|${AO_FORBIDDEN_AUDIO_CONTROL_INCLUDE_REGEX})")
set(AO_FORBIDDEN_GTK_CORE_INCLUDE_REGEX "#[ \t]*include[ \t]*[<\\\"](ao/lmdb/|ao/library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView))")
set(AO_LINUX_GTK_BOUNDARY_REPORT_EXCLUDE_REGEX [=[/platform/|/MainWindowCoordinator\.cpp|/AudioBackendBootstrap\.]=])

add_custom_target(ao_rt_public_boundary_report
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/include/ao/rt"
          "-DFORBIDDEN_REGEX=${AO_FORBIDDEN_RT_PUBLIC_INCLUDE_REGEX}"
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
          "-DFORBIDDEN_REGEX=${AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/uimodel"
          "-DFORBIDDEN_REGEX=${AO_FORBIDDEN_UIMODEL_CORE_INCLUDE_REGEX}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking storage/control-plane includes in ao_app_uimodel"
  VERBATIM
)

add_dependencies(ao_app_uimodel ao_uimodel_platform_boundary_report ao_uimodel_core_boundary_report)

add_custom_target(ao_linux_gtk_core_boundary_report
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app/linux-gtk"
          "-DFORBIDDEN_REGEX=${AO_FORBIDDEN_GTK_CORE_INCLUDE_REGEX}"
          "-DEXCLUDE_REGEX=${AO_LINUX_GTK_BOUNDARY_REPORT_EXCLUDE_REGEX}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking direct storage/library includes in linux-gtk"
  VERBATIM
)

add_custom_target(ao_layering_boundary_report
  DEPENDS
    ao_rt_public_boundary_report
    ao_uimodel_platform_boundary_report
    ao_uimodel_core_boundary_report
    ao_linux_gtk_core_boundary_report
)

# Configure-time check: ao_app_uimodel must not link platform UI targets
get_target_property(_ao_uimodel_links ao_app_uimodel LINK_LIBRARIES)
foreach(_lib IN LISTS _ao_uimodel_links)
  if(_lib MATCHES "PkgGTKMM|gtkmm|gdkmm|giomm|glibmm|WinUI|WindowsApp")
    message(FATAL_ERROR
      "ao_app_uimodel must not link platform UI libraries (found: ${_lib})")
  endif()
endforeach()

# Frontends
add_subdirectory(linux-gtk)
add_subdirectory(cli)

# Placeholders for future platforms
# add_subdirectory(windows-winui)
# add_subdirectory(tui)
