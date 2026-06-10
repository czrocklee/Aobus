# app/runtime: Shared application runtime (UI-neutral, used by all frontends)
add_library(ao_app_runtime STATIC
    runtime/AppRuntime.cpp
    runtime/CoreRuntime.cpp
    runtime/LibraryYamlImporter.cpp
    runtime/LibraryYamlExporter.cpp
    runtime/AllTracksSource.cpp
    runtime/ConfigStore.cpp
    runtime/PlaybackService.cpp
    runtime/LibraryMutationService.cpp
    runtime/ListSourceStore.cpp
    runtime/ManualListSource.cpp
    runtime/NavigationHistory.cpp
    runtime/WorkspaceService.cpp
    runtime/NotificationService.cpp
    runtime/TrackCommandService.cpp
    runtime/SmartListEvaluator.cpp
    runtime/SmartListSource.cpp
    runtime/TrackPresentation.cpp
    runtime/TrackField.cpp
    runtime/TrackFieldReader.cpp
    runtime/TrackListProjection.cpp
    runtime/TrackDetailProjection.cpp
    runtime/TrackSource.cpp
    runtime/ViewService.cpp
)
target_include_directories(ao_app_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ao_app_runtime PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_runtime PUBLIC ao ao_async PkgRapidYaml)
target_link_libraries(ao_app_runtime PRIVATE Boost::headers)

# ao_app_uimodel: platform-neutral UI models (reusable by linux-gtk, WinUI 3, tests)
add_library(ao_app_uimodel STATIC
    uimodel/layout/ActionCatalog.cpp
    uimodel/layout/ActionValidator.cpp
    uimodel/layout/ComponentCatalog.cpp
    uimodel/layout/LayoutDocument.cpp
    uimodel/layout/LayoutTemplateExpander.cpp
    uimodel/layout/LayoutValue.cpp
    uimodel/list/ListActionPolicy.cpp
    uimodel/list/SmartListEditorModel.cpp
    uimodel/playback/AobusSoulViewModel.cpp
    uimodel/playback/AudioOutputViewModel.cpp
    uimodel/playback/AudioQualityFormat.cpp
    uimodel/playback/NowPlayingViewModel.cpp
    uimodel/playback/PlaybackPositionInterpolator.cpp
    uimodel/playback/PlaybackQueueModel.cpp
    uimodel/playback/PlaybackTimeViewModel.cpp
    uimodel/playback/SeekViewModel.cpp
    uimodel/playback/TransportViewModel.cpp
    uimodel/playback/VolumeViewModel.cpp
    uimodel/track/TrackPageRoute.cpp
    uimodel/track/TrackFilterViewModel.cpp
    uimodel/track/TrackFilterResolver.cpp
    uimodel/track/TrackPresentationViewModel.cpp
    uimodel/track/TrackPresentationRecommender.cpp
    uimodel/track/TrackFieldFormatter.cpp
    uimodel/tag/TagEditWorkflow.cpp
)
target_include_directories(ao_app_uimodel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ao_app_uimodel PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_uimodel PUBLIC ao ao_app_runtime)
target_link_libraries(ao_app_uimodel PRIVATE)

# --- Boundary guardrails for ao_app_uimodel ---
# Ensure shared UI-model layout files never include platform UI headers
add_custom_target(ao_uimodel_layout_boundary_check
  COMMAND ${CMAKE_COMMAND}
          -DROOTS="${CMAKE_SOURCE_DIR}/app/include/ao/uimodel/layout;${CMAKE_SOURCE_DIR}/app/uimodel/layout"
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking for forbidden platform UI includes in ao_app_uimodel layout files"
)

add_dependencies(ao_app_uimodel ao_uimodel_layout_boundary_check)

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
