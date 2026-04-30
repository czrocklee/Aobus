# app_linux: Linux platform implementation
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
target_link_libraries(app_linux PRIVATE rs rs_model rs_audio GTKMM glibmm-2.68)
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

if(CLANG_TIDY_EXECUTABLE)
  set_target_properties(app_linux PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_STRICT_CHECKS};--config=${CLANG_TIDY_STRICT_CONFIG};-header-filter=${CLANG_TIDY_STRICT_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
  )
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