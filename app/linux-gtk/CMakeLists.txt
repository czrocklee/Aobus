# aobus-gtk-lib: GTK implementation for Linux
add_library(aobus-gtk-lib STATIC
    ui/MainWindow.cpp
    ui/TrackPageGraph.cpp
    ui/PlaybackController.cpp
    ui/InspectorSidebar.cpp
    ui/CoverArtCache.cpp
    ui/CoverArtWidget.cpp
    ui/ImportExportCoordinator.cpp
    ui/ListSidebarController.cpp
    ui/TagEditController.cpp
    ui/SessionPersistence.cpp
    ui/TrackPresentation.cpp
    ui/TrackViewPage.cpp
    ui/GtkMainThreadDispatcher.cpp
    ui/TrackListAdapter.cpp
    ui/TrackRow.cpp
    ui/ListRow.cpp
    ui/ListTreeNode.cpp
    ui/SmartListDialog.cpp
    ui/QueryExpressionBox.cpp
    ui/TagPromptDialog.cpp
    ui/TagPopover.cpp
    ui/TagEditor.cpp
    ui/ImportProgressDialog.cpp
    ui/StatusBar.cpp
    ui/TrackRowDataProvider.cpp
    ui/PlaybackBar.cpp
    ui/AobusSoul.cpp
    ui/AobusSoulWindow.cpp
    ui/VolumeBar.cpp
    ui/OutputMenuModel.cpp
    ui/ThemeBus.cpp
    ui/GtkControlExecutor.cpp

    services/PlaylistExporter.cpp
)

target_link_libraries(aobus-gtk-lib PUBLIC app_common app_runtime)
target_link_libraries(aobus-gtk-lib PRIVATE ao ao_audio PkgGTKMM glibmm-2.68 Boost::headers)
target_include_directories(aobus-gtk-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(aobus-gtk-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ui")
target_include_directories(aobus-gtk-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/services")

aobus_apply_clang_tidy(aobus-gtk-lib STRICT)

# aobus-gtk executable
add_executable(aobus-gtk main.cpp)
target_link_libraries(aobus-gtk PRIVATE aobus-gtk-lib ao PkgGTKMM glibmm-2.68 mimalloc)
target_include_directories(aobus-gtk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Configure ASan leak detection
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
  set(ASAN_SUPPRESSIONS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../.asan_suppressions")
  set(ASAN_ENV "LSAN_OPTIONS=suppressions=${ASAN_SUPPRESSIONS_FILE}")
  set_target_properties(aobus-gtk PROPERTIES ENVIRONMENT "${ASAN_ENV}")
endif()
