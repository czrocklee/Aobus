# aobus-gtk-lib: GTK implementation for Linux
add_library(aobus-gtk-lib STATIC
    app/AobusSoul.cpp
    app/GtkControlExecutor.cpp
    app/MainWindow.cpp
    app/MenuController.cpp
    app/ShellLayoutController.cpp
    app/ThemeBus.cpp
    app/WindowController.cpp

    track/LibraryTrackCountLabel.cpp
    track/SelectionInfoLabel.cpp
    track/StatusNotificationLabel.cpp
    track/TrackPageManager.cpp
    track/TrackViewPage.cpp
    track/TrackColumnController.cpp
    track/TrackFilterController.cpp
    track/TrackSelectionController.cpp
    track/TrackListAdapter.cpp
    track/TrackListModel.cpp
    track/TrackPresentation.cpp
    track/TrackRowObject.cpp
    track/TrackRowCache.cpp
    track/TrackColumnFactoryBuilder.cpp

    list/ListSidebarController.cpp
    list/ListSidebarPanel.cpp
    list/ListTreeModelBuilder.cpp
    list/ListRowObject.cpp
    list/ListTreeItem.cpp
    list/SmartListDialog.cpp
    list/QueryExpressionBox.cpp

    playback/AobusSoulBinding.cpp
    playback/NowPlayingFieldLabel.cpp
    playback/NowPlayingStatusLabel.cpp
    playback/PlaybackDetailsWidget.cpp
    playback/PlaybackSequenceController.cpp
    playback/PlaybackPositionInterpolator.cpp
    playback/TransportButton.cpp
    playback/SeekControl.cpp
    playback/TimeLabel.cpp
    playback/OutputSelector.cpp
    playback/VolumeControl.cpp
    playback/AobusSoulWindow.cpp
    playback/VolumeBar.cpp

    inspector/TrackInspectorPanel.cpp
    inspector/CoverArtCache.cpp
    inspector/CoverArtWidget.cpp

    tag/TagEditController.cpp
    tag/TagPopover.cpp
    tag/TagEditor.cpp

    library_io/ImportExportCoordinator.cpp
    library_io/ImportProgressDialog.cpp
    library_io/ImportProgressIndicator.cpp
    library_io/PlaylistExporter.cpp

    layout/document/LayoutDocument.cpp
    layout/runtime/LayoutRuntime.cpp
    layout/runtime/ComponentRegistry.cpp
    layout/runtime/LayoutHost.cpp
    layout/components/Containers.cpp
    layout/components/PlaybackComponents.cpp
    layout/components/SemanticComponents.cpp
    layout/components/StatusComponents.cpp
    layout/editor/LayoutEditorDialog.cpp
)

target_link_libraries(aobus-gtk-lib PUBLIC app_runtime)
target_link_libraries(aobus-gtk-lib PRIVATE ao ao_audio PkgGTKMM glibmm-2.68 Boost::headers)
target_include_directories(aobus-gtk-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

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
