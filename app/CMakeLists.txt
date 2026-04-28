# app_core: platform-agnostic application logic
add_library(app_core STATIC)
target_include_directories(app_core PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_sources(app_core PRIVATE
    core/AudioFormat.h
    core/IMainThreadDispatcher.h
    core/Log.cpp
    core/Log.h
    core/ImportWorker.cpp
    core/ImportWorker.h

    # backend
    core/backend/BackendTypes.h
    core/backend/IAudioBackend.h
    core/backend/IBackendManager.h
    core/backend/NullBackend.h

    # decoder
    core/decoder/DecoderTypes.h
    core/decoder/IAudioDecoderSession.h
    core/decoder/AudioDecoderFactory.cpp
    core/decoder/AudioDecoderFactory.h
    core/decoder/FlacDecoderSession.cpp
    core/decoder/FlacDecoderSession.h
    core/decoder/AlacDecoderSession.cpp
    core/decoder/AlacDecoderSession.h

    # model
    core/model/TrackIdList.cpp
    core/model/TrackIdList.h
    core/model/AllTrackIdsList.cpp
    core/model/AllTrackIdsList.h
    core/model/FilteredTrackIdList.cpp
    core/model/FilteredTrackIdList.h
    core/model/ListDraft.h
    core/model/ManualTrackIdList.cpp
    core/model/ManualTrackIdList.h
    core/model/SmartListEngine.cpp
    core/model/SmartListEngine.h

    # playback
    core/playback/PlaybackTypes.h
    core/playback/PlaybackController.cpp
    core/playback/PlaybackController.h
    core/playback/PlaybackEngine.cpp
    core/playback/PlaybackEngine.h
    core/playback/FormatNegotiator.cpp
    core/playback/FormatNegotiator.h

    # source
    core/source/IPcmSource.h
    core/source/MemoryPcmSource.cpp
    core/source/MemoryPcmSource.h
    core/source/PcmRingBuffer.cpp
    core/source/PcmRingBuffer.h
    core/source/StreamingPcmSource.cpp
    core/source/StreamingPcmSource.h

    # util
    core/util/ThreadUtils.h
)
target_link_libraries(app_core PUBLIC rs)
target_link_libraries(app_core PRIVATE PkgFLAC PkgALAC spdlog::spdlog yaml-cpp)

# app_linux: Linux platform implementation (depends on app_core)
add_library(app_linux STATIC)
target_sources(app_linux PRIVATE
    platform/linux/AppConfig.cpp
    platform/linux/AppConfig.h

    platform/linux/ui/MainWindow.cpp
    platform/linux/ui/TrackPresentation.cpp
    platform/linux/ui/TrackViewPage.cpp
    platform/linux/ui/GtkMainThreadDispatcher.cpp
    platform/linux/ui/TrackListAdapter.cpp
    platform/linux/ui/TrackRow.cpp
    platform/linux/ui/ListRow.cpp
    platform/linux/ui/ListTreeNode.cpp
    platform/linux/ui/SmartListDialog.cpp
    platform/linux/ui/QueryExpressionBox.cpp
    platform/linux/ui/TagPromptDialog.cpp
    platform/linux/ui/TagPopover.cpp
    platform/linux/ui/ImportProgressDialog.cpp
    platform/linux/ui/StatusBar.cpp
    platform/linux/ui/TrackRowDataProvider.cpp
    platform/linux/ui/PlaybackBar.cpp
    platform/linux/ui/OutputMenuModel.cpp

    platform/linux/services/PlaylistExporter.cpp
)
target_link_libraries(app_linux PRIVATE app_core GTKMM glibmm-2.68)
target_include_directories(app_linux PRIVATE "${CMAKE_SOURCE_DIR}/app")

if(PIPEWIRE_FOUND)
  target_compile_definitions(app_linux PRIVATE PIPEWIRE_FOUND)
  target_sources(app_linux PRIVATE
    platform/linux/playback/PipeWireBackend.cpp
    platform/linux/playback/PipeWireManager.cpp
    platform/linux/playback/PipeWireMonitor.cpp
    platform/linux/playback/PipeWireMonitor.h
    platform/linux/playback/detail/PipeWireShared.h
    platform/linux/playback/detail/PipeWireShared.cpp
  )
  target_link_libraries(app_linux PRIVATE PipeWire)
endif()

if(ALSA_FOUND)
  target_compile_definitions(app_linux PRIVATE ALSA_FOUND)
  target_sources(app_linux PRIVATE
    platform/linux/playback/AlsaExclusiveBackend.cpp
    platform/linux/playback/AlsaManager.cpp
  )
  target_link_libraries(app_linux PRIVATE ALSA UDEV)
endif()

# RockStudio executable (links app_linux, no direct source compilation)
add_executable(RockStudio platform/linux/main.cpp)
target_link_libraries(RockStudio PRIVATE app_linux rs GTKMM glibmm-2.68)
target_include_directories(RockStudio PRIVATE "${CMAKE_SOURCE_DIR}/app")

# Configure ASan leak detection to use suppression file for third-party libraries (RockStudio app only)
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
  set(ASAN_SUPPRESSIONS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/.asan_suppressions")
  set(ASAN_ENV "LSAN_OPTIONS=suppressions=${ASAN_SUPPRESSIONS_FILE}")
  set_target_properties(RockStudio PROPERTIES ENVIRONMENT "${ASAN_ENV}")
endif()