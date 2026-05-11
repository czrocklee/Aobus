# app/runtime: Shared application runtime (UI-neutral, used by all frontends)
add_library(app_runtime STATIC
    runtime/AppSession.cpp
    runtime/AppSession.h
    runtime/AllTracksSource.cpp
    runtime/AllTracksSource.h
    runtime/CorePrimitives.h
    runtime/ConfigStore.cpp
    runtime/ConfigStore.h
    runtime/ProjectionTypes.h
    runtime/PlaybackService.cpp
    runtime/PlaybackService.h
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
    runtime/SmartListEvaluator.cpp
    runtime/SmartListEvaluator.h
    runtime/SmartListSource.cpp
    runtime/SmartListSource.h
    runtime/TrackListPresentation.cpp
    runtime/TrackListPresentation.h
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
target_link_libraries(app_runtime PRIVATE ao yaml-cpp::yaml-cpp)
aobus_apply_clang_tidy(app_runtime STRICT)

# Frontends
add_subdirectory(linux-gtk)
add_subdirectory(cli)

# Placeholders for future platforms
# add_subdirectory(windows-winui)
# add_subdirectory(tui)