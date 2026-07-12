add_library(aobus-tui-lib STATIC
    AnchoredOverlay.cpp
    CommandCompletion.cpp
    CommandPalettePanel.cpp
    CommandCompletionProvider.cpp
    CommandCompletionState.cpp
    EventController.cpp
    Executor.cpp
    FrameTimer.cpp
    LibraryController.cpp
    ListNavigation.cpp
    NotificationCenterPanel.cpp
    OutputDeviceController.cpp
    OutputDevicePanel.cpp
    PlaybackActions.cpp
    PlaybackPanel.cpp
    PresentationPanel.cpp
    QualityPanel.cpp
    Render.cpp
    SelectableList.cpp
    StatusBar.cpp
    Style.cpp
    TrackTable.cpp
    TuiHitRegions.cpp
    LibraryNavigation.cpp
    PlaybackStatusFormatter.cpp
    QualityIndicatorStyle.cpp
    SelectionNavigation.cpp
    TrackDetailLines.cpp
    TrackListEntry.cpp
    TrackPresentationNavigation.cpp
    TrackSection.cpp
    ShellInteractionModel.cpp
    SoulButton.cpp
    TextCell.cpp
    CoverArt.cpp
)

if(WIN32)
    target_sources(aobus-tui-lib PRIVATE SignalExitWatcherWindows.cpp)
else()
    target_sources(aobus-tui-lib PRIVATE SignalExitWatcherPosix.cpp)
endif()

target_include_directories(aobus-tui-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aobus-tui-lib PUBLIC ao_app_runtime ao_app_uimodel ftxui::component)
target_link_libraries(aobus-tui-lib PRIVATE PkgStb)

add_executable(aobus-tui
    AudioBackendBootstrap.cpp
    Main.cpp
    App.cpp
)

target_link_libraries(aobus-tui PRIVATE
    aobus-tui-lib
    ao_app_uimodel
    ao_app_runtime
    ao
    ao_audio
    CLI11::CLI11
    ftxui::component
    ftxui::dom
    ftxui::screen
    mimalloc
)

target_include_directories(aobus-tui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
