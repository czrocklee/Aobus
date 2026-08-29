# Prepared-next contract diagnostics are compiled once and shared with the
# minimal fatal probe so source-location coverage does not duplicate a source
# file across targets with incompatible compile commands.
add_library(ao_prepared_next_contract STATIC
    playback/PreparedNextContract.cpp
)
target_include_directories(ao_prepared_next_contract PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_prepared_next_contract PRIVATE ao_utility)

# app/runtime: Shared application runtime (UI-neutral, used by all frontends)
add_library(ao_app_runtime STATIC
    AppRuntime.cpp
    AppState.cpp
    CoreRuntime.cpp
    library/Library.cpp
    library/LibraryPaths.cpp
    library/LibraryWriteLane.cpp
    library/LibrarySnapshot.cpp
    library/LibraryChanges.cpp
    library/MediaTrack.cpp
    library/TrackBuilderSnapshot.cpp
    library/LibraryYamlImporter.cpp
    library/LibraryYamlExporter.cpp
    completion/CompletionService.cpp
    completion/CompletionText.cpp
    completion/QueryExpressionCompleter.cpp
    completion/MetadataValueCompleter.cpp
    source/AllTracksSource.cpp
    source/IndexedTrackSequence.cpp
    source/CachedListSource.cpp
    ConfigStore.cpp
    playback/PlaybackTransport.cpp
    playback/PlaybackSuccession.cpp
    PlaybackSessionYamlSchema.cpp
    PlaybackSessionPersistence.cpp
    PlaybackSessionState.cpp
    playback/PlaybackService.cpp
    playback/PlaybackCursor.cpp
    playback/PlaybackCursorSession.cpp
    playback/PlaybackRestartDeadline.cpp
    playback/PreparedNextRegistry.cpp
    playback/ProjectionAnchor.cpp
    playback/ShuffleHistory.cpp
    library/LibraryCommands.cpp
    library/AudioIdentityBatchWriter.cpp
    library/AudioIdentityIndexer.cpp
    library/LibraryScan.cpp
    library/ScanPlan.cpp
    library/ScanApplyOperation.cpp
    library/LibraryJobs.cpp
    resource/ResourceByteMemoryCache.cpp
    resource/ResourceCarrierIndex.cpp
    resource/ResourceByteDiskCache.cpp
    resource/ResourceByteReader.cpp
    source/TrackSourceCache.cpp
    source/ListOrderSource.cpp
    NavigationHistory.cpp
    WorkspaceSessionYamlSchema.cpp
    WorkspaceService.cpp
    NotificationService.cpp
    WritableTagList.cpp
    source/SmartListEvaluator.cpp
    source/SmartListSource.cpp
    TrackPresentation.cpp
    TrackField.cpp
    TrackFieldReader.cpp
    TrackEditScript.cpp
    projection/TrackListProjection.cpp
    projection/StringArena.cpp
    projection/TrackProjectionEditScript.cpp
    projection/TrackDetailProjection.cpp
    source/TrackSource.cpp
    source/TrackSourceDelta.cpp
    source/TrackSourceDeltaBuilder.cpp
    source/TrackSourceLease.cpp
    ScopedTimer.cpp
    ViewService.cpp
    Log.cpp
)

# GCC 15 misclassifies Boost.Asio coroutine-frame aligned_alloc/free pairs in
# these coroutine-heavy translation units after inlining. Keep the workaround
# local so unrelated runtime and frontend code retains the warning.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  set_property(
    SOURCE
      library/LibraryJobs.cpp
      library/LibraryCommands.cpp
    APPEND PROPERTY COMPILE_OPTIONS -Wno-mismatched-new-delete)
endif()

target_include_directories(ao_app_runtime PUBLIC "${CMAKE_SOURCE_DIR}/app/include")
target_include_directories(ao_app_runtime PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_runtime PUBLIC ao ao_async PkgRapidYaml spdlog::spdlog)
target_link_libraries(ao_app_runtime PRIVATE ao_prepared_next_contract ao_unicode_text Boost::headers)
