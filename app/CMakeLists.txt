# app/runtime: Shared application runtime (UI-neutral, used by all frontends)
add_library(app_runtime STATIC
    runtime/AppRuntime.cpp
    runtime/AppRuntime.h
    runtime/async/Runtime.cpp
    runtime/async/Runtime.h
    runtime/async/Task.h
    runtime/async/LifetimeScope.cpp
    runtime/async/LifetimeScope.h
    runtime/CoreRuntime.cpp
    runtime/CoreRuntime.h
    runtime/LibraryYamlImporter.cpp
    runtime/LibraryYamlImporter.h
    runtime/LibraryYamlExporter.cpp
    runtime/LibraryYamlExporter.h
    runtime/AllTracksSource.cpp
    runtime/AllTracksSource.h
    runtime/CorePrimitives.h
    runtime/ConfigStore.cpp
    runtime/ConfigStore.h
    runtime/ProjectionTypes.h
    runtime/PlaybackService.cpp
    runtime/PlaybackService.h
    runtime/SessionPersistenceService.cpp
    runtime/SessionPersistenceService.h
    runtime/LibraryMutationService.cpp
    runtime/LibraryMutationService.h
    runtime/ListSourceStore.cpp
    runtime/ListSourceStore.h
    runtime/ManualListSource.cpp
    runtime/ManualListSource.h
    runtime/WorkspaceService.cpp
    runtime/WorkspaceService.h
    runtime/NotificationService.cpp
    runtime/NotificationService.h
    runtime/StateTypes.h
    runtime/TrackCommandService.cpp
    runtime/TrackCommandService.h
    runtime/SmartListEvaluator.cpp
    runtime/SmartListEvaluator.h
    runtime/SmartListSource.cpp
    runtime/SmartListSource.h
    runtime/TrackPresentationPreset.cpp
    runtime/TrackPresentationPreset.h
    runtime/TrackField.cpp
    runtime/TrackField.h
    runtime/TrackListProjection.cpp
    runtime/TrackListProjection.h
    runtime/TrackDetailProjection.cpp
    runtime/TrackDetailProjection.h
    runtime/TrackSource.cpp
    runtime/TrackSource.h
    runtime/ViewService.cpp
    runtime/ViewService.h
)
target_include_directories(app_runtime PUBLIC "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(app_runtime PUBLIC PkgRapidYaml)
target_link_libraries(app_runtime PRIVATE ao Boost::headers)

# Frontends
add_subdirectory(linux-gtk)
add_subdirectory(cli)

# Placeholders for future platforms
# add_subdirectory(windows-winui)
# add_subdirectory(tui)