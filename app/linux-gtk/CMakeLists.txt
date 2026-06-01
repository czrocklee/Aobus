# GResource compilation with automatic dependency tracking
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/resources.c
    COMMAND ${GLIB_COMPILE_RESOURCES_EXECUTABLE}
            --target=${CMAKE_CURRENT_BINARY_DIR}/resources.c
            --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}
            --generate-source
            --dependency-file=${CMAKE_CURRENT_BINARY_DIR}/resources.c.d
            ${CMAKE_CURRENT_SOURCE_DIR}/aobus.gresource.xml
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/aobus.gresource.xml
    DEPFILE ${CMAKE_CURRENT_BINARY_DIR}/resources.c.d
    COMMENT "Compiling GResources for aobus-gtk"
    VERBATIM
)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/resources.c PROPERTIES COMPILE_OPTIONS "-Wno-overlength-strings")

# aobus-gtk-lib: GTK implementation for Linux
add_library(aobus-gtk-lib STATIC
    ${CMAKE_CURRENT_BINARY_DIR}/resources.c
    app/AobusSoul.cpp
    app/AppDialog.cpp
    app/FormBuilder.cpp
    app/GtkControlExecutor.cpp
    app/MainWindow.cpp
    app/MenuController.cpp
    app/ShellLayoutController.cpp
    app/StyleManager.cpp
    app/MainWindowCoordinator.cpp
    app/AppConfig.cpp
    app/GtkLayoutConfig.cpp
    platform/AudioBackendBootstrap.cpp

    track/LibraryTrackCountLabel.cpp
    track/SelectionInfoLabel.cpp
    track/StatusSlot.cpp
    track/TrackPageHost.cpp
    track/TrackViewPage.cpp
    track/TrackColumnController.cpp
    track/TrackColumnViewHost.cpp
    track/TrackQuickFilter.cpp
    track/TrackPresentationButton.cpp
    track/TrackSelectionController.cpp
    track/TrackListModel.cpp
    track/TrackCustomViewDialog.cpp
    track/TrackRowObject.cpp
    track/TrackRowCache.cpp
    track/TrackColumnFactoryBuilder.cpp
    track/TrackFieldUi.cpp

    list/ListNavigationController.cpp
    list/ListNavigationPanel.cpp
    list/ListTreeModelBuilder.cpp
    list/ListRowObject.cpp
    list/ListTreeItem.cpp
    list/SmartListDialog.cpp
    list/QueryExpressionBox.cpp

    playback/AobusSoulWindow.cpp
    playback/AudioQualityCss.cpp
    playback/AudioPipelinePanel.cpp
    playback/NowPlayingFieldLabel.cpp
    playback/NowPlayingStatusLabel.cpp
    playback/PlaybackDetailsWidget.cpp
    playback/TransportButton.cpp
    playback/TimeLabel.cpp
    playback/AudioDeviceSelector.cpp
    playback/VolumeControl.cpp
    playback/SeekControl.cpp
    playback/VolumeBar.cpp

    image/ImageCache.cpp
    image/ImageWidget.cpp

    tag/TagEditController.cpp
    tag/TagPopover.cpp
    tag/TagEditor.cpp
    tag/TrackPropertiesDialog.cpp

    portal/ImportExportCoordinator.cpp
    portal/LibraryTaskProgressDialog.cpp

    layout/document/GtkLayoutPresets.cpp
    layout/runtime/LayoutRuntime.cpp
    layout/runtime/ComponentRegistry.cpp
    layout/runtime/ComponentInteractionController.cpp
    layout/runtime/ComponentTooltipController.cpp
    layout/runtime/DecoratedLayoutComponent.cpp
    layout/runtime/ActionRegistry.cpp
    layout/runtime/GioActionBridge.cpp
    layout/runtime/ActionValidator.cpp
    layout/runtime/ActionBinder.cpp
    layout/runtime/LayoutHost.cpp
    layout/components/Containers.cpp
    layout/components/PlaybackComponents.cpp
    layout/components/SemanticComponents.cpp
    layout/components/StatusComponents.cpp
    layout/components/TrackComponents.cpp
    layout/components/TrackDetailComponents.cpp
    layout/components/TrackEditorComponents.cpp
    layout/editor/LayoutEditorDialog.cpp
)

target_link_libraries(aobus-gtk-lib PUBLIC ao_app_uimodel ao_app_runtime)
target_link_libraries(aobus-gtk-lib PRIVATE ao ao_audio PkgGTKMM glibmm-2.68 Boost::headers)
target_include_directories(aobus-gtk-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")


# aobus-gtk executable
add_executable(aobus-gtk main.cpp)
target_link_libraries(aobus-gtk PRIVATE aobus-gtk-lib ao PkgGTKMM glibmm-2.68 mimalloc -Wl,-u,aobus_get_resource)
target_include_directories(aobus-gtk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Configure ASan leak detection
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
  set(ASAN_SUPPRESSIONS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../.asan_suppressions")
  set(ASAN_ENV "LSAN_OPTIONS=suppressions=${ASAN_SUPPRESSIONS_FILE}")
  set_target_properties(aobus-gtk PROPERTIES ENVIRONMENT "${ASAN_ENV}")
endif()
