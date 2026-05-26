# app/runtime: Shared application runtime (UI-neutral, used by all frontends)
add_library(app_runtime STATIC
    runtime/AppRuntime.cpp
    runtime/async/Runtime.cpp
    runtime/async/LifetimeScope.cpp
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
    runtime/TrackListProjection.cpp
    runtime/TrackDetailProjection.cpp
    runtime/TrackSource.cpp
    runtime/ViewService.cpp
)
target_include_directories(app_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(app_runtime PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(app_runtime PUBLIC PkgRapidYaml)
target_link_libraries(app_runtime PRIVATE ao Boost::headers)

# ao_app_uimodel: platform-neutral UI models (reusable by linux-gtk, WinUI 3, tests)
add_library(ao_app_uimodel STATIC
    uimodel/list/ListActionPolicy.cpp
    uimodel/list/SmartListEditorModel.cpp
    uimodel/playback/AobusSoulViewModel.cpp
    uimodel/playback/AudioOutputViewModel.cpp
    uimodel/playback/NowPlayingViewModel.cpp
    uimodel/playback/PlaybackPositionInterpolator.cpp
    uimodel/playback/PlaybackQueueModel.cpp
    uimodel/playback/PlaybackTimeViewModel.cpp
    uimodel/playback/SeekViewModel.cpp
    uimodel/playback/TransportViewModel.cpp
    uimodel/playback/VolumeViewModel.cpp
    uimodel/track/TrackPageRoute.cpp
    uimodel/track/TrackFilterViewModel.cpp
    uimodel/tag/TagEditWorkflow.cpp
)
target_include_directories(ao_app_uimodel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ao_app_uimodel PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_uimodel PUBLIC app_runtime)
target_link_libraries(ao_app_uimodel PRIVATE ao)

# Frontends
add_subdirectory(linux-gtk)
add_subdirectory(cli)

# Placeholders for future platforms
# add_subdirectory(windows-winui)
# add_subdirectory(tui)
