if(NOT WIN32)
  message(FATAL_ERROR "The WinUI frontend is available only on native Windows.")
endif()

set(AOBUS_NO_COVER_NOTE_ASSET "${CMAKE_SOURCE_DIR}/asset/ui/no-cover/note.svg")
set(AOBUS_NO_COVER_VINYL_ASSET "${CMAKE_SOURCE_DIR}/asset/ui/no-cover/vinyl.svg")
set(AOBUS_NO_COVER_EQUALIZER_ASSET "${CMAKE_SOURCE_DIR}/asset/ui/no-cover/equalizer.svg")
set(AOBUS_SOUL_MARK_ASSET "${CMAKE_SOURCE_DIR}/asset/brand/SoulMark.svg")
set(AOBUS_BRAND_LICENSE_ASSET "${CMAKE_SOURCE_DIR}/asset/brand/LICENSE.txt")
set(AOBUS_NO_COVER_ASSETS
  ${AOBUS_NO_COVER_NOTE_ASSET}
  ${AOBUS_NO_COVER_VINYL_ASSET}
  ${AOBUS_NO_COVER_EQUALIZER_ASSET}
  ${AOBUS_SOUL_MARK_ASSET}
  ${AOBUS_BRAND_LICENSE_ASSET})

add_executable(aobus-winui WIN32
  ${AOBUS_NO_COVER_ASSETS}
  App.xaml
  App.xaml.cpp
  App.xaml.h
  app/DispatcherQueueExecutor.cpp
  app/DispatcherQueueExecutor.h
  app/LibrarySession.cpp
  app/LibrarySession.h
  app/WindowsUiCoordinator.cpp
  app/WindowsUiCoordinator.h
  app/WinUiDependencies.h
  image/CoverArtPresenter.cpp
  image/CoverArtPresenter.h
  image/CoverArtPlaceholderRenderer.cpp
  image/CoverArtPlaceholderRenderer.h
  image/WindowsCoverArtLoader.cpp
  image/WindowsCoverArtLoader.h
  platform/MemoryRandomAccessStream.cpp
  platform/MemoryRandomAccessStream.h
  platform/ScopedBooleanFlag.h
  platform/SmtcBridge.cpp
  platform/SmtcBridge.h
  platform/WindowsStringResources.cpp
  platform/WindowsStringResources.h
  playback/AudioPipelineToolTip.cpp
  playback/AudioPipelineToolTip.h
  playback/AobusSoulControl.cpp
  playback/AobusSoulControl.h
  playback/MainWindowPlayback.cpp
  playback/OutputDeviceControl.cpp
  playback/OutputDeviceControl.h
  playback/PlaybackControls.cpp
  playback/PlaybackControls.h
  playback/PlaybackTimeControl.cpp
  playback/PlaybackTimeControl.h
  playback/SeekControl.cpp
  playback/SeekControl.h
  playback/SoulTransportButton.cpp
  playback/SoulTransportButton.h
  playback/TransportButton.cpp
  playback/TransportButton.h
  playback/VolumeControl.cpp
  playback/VolumeControl.h
  status/ActivityStatusControl.cpp
  status/ActivityStatusControl.h
  Project.idl
  Resources/en-US/Resources.resw
  MainWindow.xaml
  MainWindow.xaml.cpp
  MainWindow.xaml.h
  shell/MainWindowShell.cpp
  theme/WindowsThemeCoordinator.cpp
  theme/WindowsThemeCoordinator.h
  track/MainWindowTrack.cpp
  track/TrackCellItem.cpp
  track/TrackCellItem.h
  track/TrackDetailControl.cpp
  track/TrackDetailControl.h
  track/TrackItemView.cpp
  track/TrackItemView.h
  track/TrackListController.cpp
  track/TrackListController.h
  track/TrackQuickFilterControl.cpp
  track/TrackQuickFilterControl.h
  track/TrackRowItem.cpp
  track/TrackRowItem.h
  app.manifest
  pch.h
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  App.xaml
  App.xaml.cpp
  App.xaml.h
  app/DispatcherQueueExecutor.cpp
  app/DispatcherQueueExecutor.h
  app/LibrarySession.cpp
  app/LibrarySession.h
  app/WindowsUiCoordinator.cpp
  app/WindowsUiCoordinator.h
  app/WinUiDependencies.h
  image/CoverArtPresenter.cpp
  image/CoverArtPresenter.h
  image/CoverArtPlaceholderRenderer.cpp
  image/CoverArtPlaceholderRenderer.h
  image/WindowsCoverArtLoader.cpp
  image/WindowsCoverArtLoader.h
  platform/MemoryRandomAccessStream.cpp
  platform/MemoryRandomAccessStream.h
  platform/ScopedBooleanFlag.h
  platform/SmtcBridge.cpp
  platform/SmtcBridge.h
  platform/WindowsStringResources.cpp
  platform/WindowsStringResources.h
  playback/AudioPipelineToolTip.cpp
  playback/AudioPipelineToolTip.h
  playback/AobusSoulControl.cpp
  playback/AobusSoulControl.h
  playback/MainWindowPlayback.cpp
  playback/OutputDeviceControl.cpp
  playback/OutputDeviceControl.h
  playback/PlaybackControls.cpp
  playback/PlaybackControls.h
  playback/PlaybackTimeControl.cpp
  playback/PlaybackTimeControl.h
  playback/SeekControl.cpp
  playback/SeekControl.h
  playback/SoulTransportButton.cpp
  playback/SoulTransportButton.h
  playback/TransportButton.cpp
  playback/TransportButton.h
  playback/VolumeControl.cpp
  playback/VolumeControl.h
  status/ActivityStatusControl.cpp
  status/ActivityStatusControl.h
  MainWindow.xaml
  MainWindow.xaml.cpp
  MainWindow.xaml.h
  shell/MainWindowShell.cpp
  theme/WindowsThemeCoordinator.cpp
  theme/WindowsThemeCoordinator.h
  track/MainWindowTrack.cpp
  track/TrackCellItem.cpp
  track/TrackCellItem.h
  track/TrackDetailControl.cpp
  track/TrackDetailControl.h
  track/TrackItemView.cpp
  track/TrackItemView.h
  track/TrackListController.cpp
  track/TrackListController.h
  track/TrackQuickFilterControl.cpp
  track/TrackQuickFilterControl.h
  track/TrackRowItem.cpp
  track/TrackRowItem.h)
source_group("Asset" FILES ${AOBUS_NO_COVER_ASSETS})

set_source_files_properties(App.xaml PROPERTIES
  VS_XAML_TYPE ApplicationDefinition)
set_source_files_properties(MainWindow.xaml PROPERTIES
  VS_TOOL_OVERRIDE Page)
set_source_files_properties(Project.idl PROPERTIES
  VS_TOOL_OVERRIDE Midl
  VS_SETTINGS "SubType=Code;OutputDirectory=$(ProjectDir)")
set_source_files_properties(Resources/en-US/Resources.resw PROPERTIES
  VS_TOOL_OVERRIDE PRIResource)
set_source_files_properties(app.manifest PROPERTIES
  VS_TOOL_OVERRIDE Manifest)
set_source_files_properties(${AOBUS_NO_COVER_NOTE_ASSET} PROPERTIES
  VS_TOOL_OVERRIDE Content
  VS_COPY_TO_OUT_DIR PreserveNewest
  VS_SETTINGS "Link=Assets\\NoCover\\note.svg")
set_source_files_properties(${AOBUS_NO_COVER_VINYL_ASSET} PROPERTIES
  VS_TOOL_OVERRIDE Content
  VS_COPY_TO_OUT_DIR PreserveNewest
  VS_SETTINGS "Link=Assets\\NoCover\\vinyl.svg")
set_source_files_properties(${AOBUS_NO_COVER_EQUALIZER_ASSET} PROPERTIES
  VS_TOOL_OVERRIDE Content
  VS_COPY_TO_OUT_DIR PreserveNewest
  VS_SETTINGS "Link=Assets\\NoCover\\equalizer.svg")
set_source_files_properties(${AOBUS_SOUL_MARK_ASSET} PROPERTIES
  VS_TOOL_OVERRIDE Content
  VS_COPY_TO_OUT_DIR PreserveNewest
  VS_SETTINGS "Link=Assets\\Brand\\SoulMark.svg")
set_source_files_properties(${AOBUS_BRAND_LICENSE_ASSET} PROPERTIES
  VS_TOOL_OVERRIDE Content
  VS_COPY_TO_OUT_DIR PreserveNewest
  VS_SETTINGS "Link=Assets\\Brand\\LICENSE.txt")
set_source_files_properties(App.xaml.cpp PROPERTIES
  VS_SETTINGS "DependentUpon=App.xaml")
set_source_files_properties(App.xaml.h PROPERTIES
  VS_SETTINGS "DependentUpon=App.xaml")
set_source_files_properties(MainWindow.xaml.cpp PROPERTIES
  VS_SETTINGS "DependentUpon=MainWindow.xaml")
set_source_files_properties(MainWindow.xaml.h PROPERTIES
  VS_SETTINGS "DependentUpon=MainWindow.xaml")
set_source_files_properties(playback/AobusSoulControl.cpp PROPERTIES
  VS_SETTINGS "DependentUpon=AobusSoulControl.h")
set_source_files_properties(track/TrackRowItem.cpp PROPERTIES
  VS_SETTINGS "DependentUpon=TrackRowItem.h")
set_source_files_properties(track/TrackCellItem.cpp PROPERTIES
  VS_SETTINGS "DependentUpon=TrackCellItem.h")

target_precompile_headers(aobus-winui PRIVATE pch.h)
target_compile_options(aobus-winui PRIVATE /bigobj)
target_include_directories(aobus-winui PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aobus-winui PRIVATE
  ao_app_runtime
  ao_app_uimodel
  runtimeobject.lib
  shcore.lib)

set_target_properties(aobus-winui PROPERTIES
  OUTPUT_NAME Aobus
  VS_GLOBAL_AppContainerApplication false
  VS_GLOBAL_ApplicationType "Windows Store"
  VS_GLOBAL_ApplicationTypeRevision 10.0
  VS_GLOBAL_DefaultLanguage en-US
  VS_GLOBAL_DesktopCompatible true
  VS_GLOBAL_MinimumVisualStudioVersion 18.0
  VS_GLOBAL_RootNamespace Aobus
  VS_GLOBAL_TargetName Aobus
  VS_GLOBAL_WindowsTargetPlatformMinVersion 10.0.26100.0
  WindowsTargetPlatformVersion 10.0.26100.0)

aobus_enable_windows_app_sdk(aobus-winui)
