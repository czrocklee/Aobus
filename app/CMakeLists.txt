# app/rt: Shared application runtime (UI-neutral, used by all frontends)
add_library(app_runtime STATIC
    rt/AppRuntime.cpp
    rt/async/Runtime.cpp
    rt/async/LifetimeScope.cpp
    rt/CoreRuntime.cpp
    rt/LibraryYamlImporter.cpp
    rt/LibraryYamlExporter.cpp
    rt/AllTracksSource.cpp
    rt/ConfigStore.cpp
    rt/PlaybackService.cpp
    rt/LibraryMutationService.cpp
    rt/ListSourceStore.cpp
    rt/ManualListSource.cpp
    rt/WorkspaceService.cpp
    rt/NotificationService.cpp
    rt/TrackCommandService.cpp
    rt/SmartListEvaluator.cpp
    rt/SmartListSource.cpp
    rt/TrackPresentation.cpp
    rt/TrackField.cpp
    rt/TrackListProjection.cpp
    rt/TrackDetailProjection.cpp
    rt/TrackSource.cpp
    rt/ViewService.cpp
)
target_include_directories(app_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(app_runtime PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(app_runtime PUBLIC PkgRapidYaml)
target_link_libraries(app_runtime PRIVATE ao Boost::headers)

# Frontends
add_subdirectory(linux-gtk)
add_subdirectory(cli)

# Placeholders for future platforms
# add_subdirectory(windows-winui)
# add_subdirectory(tui)
