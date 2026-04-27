add_executable(RockStudio
    platform/linux/main.cpp
    core/AppConfig.cpp
    core/Log.cpp
    platform/linux/ui/MainWindow.cpp
    platform/linux/ui/TrackPresentation.cpp
    platform/linux/ui/TrackViewPage.cpp
    platform/linux/ui/TrackListAdapter.cpp
    platform/linux/ui/TrackRow.cpp
    platform/linux/ui/ListRow.cpp
    platform/linux/ui/ListTreeNode.cpp
    core/ImportWorker.cpp
    platform/linux/services/PlaylistExporter.cpp
    platform/linux/ui/SmartListDialog.cpp
    platform/linux/ui/QueryExpressionBox.cpp
    platform/linux/ui/TagPromptDialog.cpp
    platform/linux/ui/TagPopover.cpp
    platform/linux/ui/ImportProgressDialog.cpp
    platform/linux/ui/StatusBar.cpp
    core/model/TrackIdList.cpp
    core/model/AllTrackIdsList.cpp
    core/model/FilteredTrackIdList.cpp
    core/model/SmartListEngine.cpp
    core/model/ManualTrackIdList.cpp
    platform/linux/ui/TrackRowDataProvider.cpp
    platform/linux/ui/PlaybackBar.cpp
    platform/linux/ui/OutputMenuModel.cpp
    core/playback/PlaybackController.cpp
    core/playback/PlaybackEngine.cpp
    core/decoder/AudioDecoderFactory.cpp
    core/decoder/FlacDecoderSession.cpp
    core/decoder/AlacDecoderSession.cpp
    core/source/MemoryPcmSource.cpp
    core/source/PcmRingBuffer.cpp
    core/source/StreamingPcmSource.cpp
    core/playback/FormatNegotiator.cpp
)
target_link_libraries(RockStudio PRIVATE PkgFLAC PkgALAC)
if(PIPEWIRE_FOUND)
  target_compile_definitions(RockStudio PRIVATE PIPEWIRE_FOUND)
  target_sources(RockStudio PRIVATE platform/linux/playback/PipeWireBackend.cpp)
  target_link_libraries(RockStudio PRIVATE PipeWire)
endif()
if(ALSA_FOUND)
  target_compile_definitions(RockStudio PRIVATE ALSA_FOUND)
  target_sources(RockStudio PRIVATE platform/linux/playback/AlsaExclusiveBackend.cpp)
  target_link_libraries(RockStudio PRIVATE ALSA UDEV)
endif()
target_link_libraries(RockStudio PRIVATE rs GTKMM spdlog::spdlog yaml-cpp CLI11::CLI11)
target_include_directories(RockStudio PRIVATE "${CMAKE_SOURCE_DIR}/app")

# Configure ASan leak detection to use suppression file for third-party libraries (RockStudio app only)
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
  set(ASAN_SUPPRESSIONS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/.asan_suppressions")
  set(ASAN_ENV "LSAN_OPTIONS=suppressions=${ASAN_SUPPRESSIONS_FILE}")
  set_target_properties(RockStudio PROPERTIES ENVIRONMENT "${ASAN_ENV}")
endif()
