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
    app/GtkAccelTranslator.cpp
    app/GtkMainContextExecutor.cpp
    app/KeyboardShortcutsWindow.cpp
    app/KeymapApplicator.cpp
    app/MainWindow.cpp
    app/MenuController.cpp
    app/MouseNavigationPolicy.cpp
    app/PlaybackShortcutPolicy.cpp
    app/ShellLayoutController.cpp
    app/ShellLayoutComponentStateStore.cpp
    app/GtkStyleRuntime.cpp
    app/ThemeCoordinator.cpp
    app/ThemePreset.cpp
    app/MainWindowCoordinator.cpp
    app/AppConfig.cpp
    app/ShellLayoutStore.cpp
    app/GtkLayoutConfig.cpp
    platform/AudioBackendBootstrap.cpp

    common/DismissController.cpp

    completion/EntryCompletionController.cpp

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
    playback/OutputListItems.cpp
    playback/PlaybackDetailsWidget.cpp
    playback/TransportButton.cpp
    playback/TimeLabel.cpp
    playback/AudioDeviceSelector.cpp
    playback/VolumeControl.cpp
    playback/SeekControl.cpp
    playback/VolumeBar.cpp

    image/ImageCache.cpp
    image/ResourceImageController.cpp
    image/ImageWidget.cpp
    image/ThumbnailLoader.cpp

    tag/TagEditController.cpp
    tag/TagPopover.cpp
    tag/TagEditor.cpp
    tag/TrackPropertiesDialog.cpp

    portal/ImportExportCoordinator.cpp
    portal/LibraryImportExportWorkflow.cpp
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
    layout/component/common/CommonLayoutProps.cpp
    layout/component/track/TrackDetailSizing.cpp
    layout/component/track/TrackRegistry.cpp
    layout/component/track/TrackQuickFilterComponent.cpp
    layout/component/track/TrackPresentationButtonComponent.cpp
    layout/component/track/TrackDetailScope.cpp
    layout/component/track/TrackSelectionRegionComponent.cpp
    layout/component/track/TrackCoverArtComponent.cpp
    layout/component/track/TrackFieldGridCustomControls.cpp
    layout/component/track/TrackFieldGridComponent.cpp
    layout/component/track/TrackFieldGridRows.cpp
    layout/component/track/TrackFieldGridTextUtils.cpp
    layout/component/track/TrackFieldGridWidgets.cpp
    layout/component/track/TrackTagEditorComponent.cpp
    layout/component/playback/AudioPipelinePanelComponent.cpp
    layout/component/playback/NowPlayingFieldComponent.cpp
    layout/component/playback/OutputSelectorComponent.cpp
    layout/component/playback/PlaybackImageComponent.cpp
    layout/component/playback/PlaybackRegistry.cpp
    layout/component/playback/QualityIndicatorComponent.cpp
    layout/component/playback/SeekSliderComponent.cpp
    layout/component/playback/SoulButtonComponent.cpp
    layout/component/playback/SoulTransportButtonComponent.cpp
    layout/component/playback/TimeLabelComponent.cpp
    layout/component/playback/TransportButtonComponent.cpp
    layout/component/playback/VolumeControlComponent.cpp
    layout/component/semantic/ActionButtonComponent.cpp
    layout/component/semantic/LabelComponent.cpp
    layout/component/semantic/ListTreeComponent.cpp
    layout/component/semantic/MenuBarComponent.cpp
    layout/component/semantic/MenuButtonComponent.cpp
    layout/component/semantic/OpenLibraryButtonComponent.cpp
    layout/component/semantic/SemanticRegistry.cpp
    layout/component/semantic/TracksTableComponent.cpp
    layout/component/semantic/WorkspaceWithDetailPaneComponent.cpp
    layout/component/container/AbsoluteCanvasComponent.cpp
    layout/component/container/BoxComponent.cpp
    layout/component/container/CenterBoxComponent.cpp
    layout/component/container/CollapsibleSplitComponent.cpp
    layout/component/container/ContainerRegistry.cpp
    layout/component/container/ResponsiveClassComponent.cpp
    layout/component/container/ScrollComponent.cpp
    layout/component/container/SeparatorComponent.cpp
    layout/component/container/SpacerComponent.cpp
    layout/component/container/TabsComponent.cpp
    layout/component/container/SplitComponent.cpp
    layout/component/status/LibraryTrackCountComponent.cpp
    layout/component/status/NowPlayingStatusComponent.cpp
    layout/component/status/PlaybackDetailsComponent.cpp
    layout/component/status/StatusMessageLabelComponent.cpp
    layout/component/status/StatusRegistry.cpp
    layout/component/status/StatusSlotComponent.cpp
    layout/editor/LayoutEditorDialog.cpp
)

target_link_libraries(aobus-gtk-lib PUBLIC ao_app_uimodel ao_app_runtime)
target_link_libraries(aobus-gtk-lib PRIVATE ao ao_audio PkgGTKMM glibmm-2.68 Boost::headers)
target_include_directories(aobus-gtk-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(aobus-gtk-lib ao_linux_gtk_core_boundary_report)


# aobus-gtk executable
add_executable(aobus-gtk main.cpp)
target_link_libraries(aobus-gtk PRIVATE aobus-gtk-lib ao PkgGTKMM glibmm-2.68 mimalloc -Wl,-u,aobus_get_resource)
target_include_directories(aobus-gtk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
