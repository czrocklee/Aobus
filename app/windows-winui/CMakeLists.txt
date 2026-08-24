if(NOT WIN32)
  message(FATAL_ERROR "The WinUI frontend is available only on native Windows.")
endif()

set(AOBUS_NO_COVER_NOTE_ASSET "${CMAKE_SOURCE_DIR}/asset/ui/no-cover/note.svg")
set(AOBUS_NO_COVER_VINYL_ASSET "${CMAKE_SOURCE_DIR}/asset/ui/no-cover/vinyl.svg")
set(AOBUS_NO_COVER_EQUALIZER_ASSET "${CMAKE_SOURCE_DIR}/asset/ui/no-cover/equalizer.svg")
set(AOBUS_MODERN_LAYOUT_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/layout/windows_modern_layout.yaml")
set(AOBUS_CLASSIC_LAYOUT_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/layout/windows_classic_layout.yaml")
set(AOBUS_SOUL_MARK_ASSET "${CMAKE_SOURCE_DIR}/asset/brand/SoulMark.svg")
set(AOBUS_BRAND_LICENSE_ASSET "${CMAKE_SOURCE_DIR}/asset/brand/LICENSE.txt")
set(AOBUS_NO_COVER_ASSETS
  ${AOBUS_NO_COVER_NOTE_ASSET}
  ${AOBUS_NO_COVER_VINYL_ASSET}
  ${AOBUS_NO_COVER_EQUALIZER_ASSET}
  ${AOBUS_SOUL_MARK_ASSET}
  ${AOBUS_BRAND_LICENSE_ASSET})
set(AOBUS_SHELL_LAYOUT_ASSETS
  ${AOBUS_MODERN_LAYOUT_ASSET}
  ${AOBUS_CLASSIC_LAYOUT_ASSET})

# All compiled Windows implementation belongs to one frontend library. The
# executable below is only the final link and deployed-resource boundary; WinUI
# generates its application entry point while compiling App.xaml in the library.
set(AOBUS_WINUI_LIBRARY_SOURCES
  App.xaml
  App.xaml.cpp
  App.xaml.h
  app/DestructiveLibraryRestart.cpp
  app/DesktopOutputSelection.cpp
  app/DesktopSettingsYamlSchema.cpp
  app/DispatcherQueueAdmission.cpp
  app/DispatcherQueueAdmission.h
  app/DispatcherQueueExecutor.cpp
  app/DispatcherQueueExecutor.h
  app/LibraryWindowSession.cpp
  app/LibraryWindowSession.h
  app/LibrarySession.cpp
  app/LibrarySession.h
  app/SelectedRootCommit.cpp
  include/ao/winui/app/DesktopOutputSelection.h
  app/UiCoordinator.cpp
  app/UiCoordinator.h
  image/CoverArtPresenter.cpp
  image/CoverArtPresenter.h
  image/CoverArtPlaceholderRenderer.cpp
  image/CoverArtPlaceholderRenderer.h
  input/KeyChordAccelerator.cpp
  input/KeymapAcceleratorPlan.cpp
  input/KeymapAccelerators.cpp
  input/KeymapAccelerators.h
  input/SystemCharacterKey.cpp
  input/SystemCharacterKey.h
  include/ao/winui/input/KeyChordAccelerator.h
  include/ao/winui/input/KeymapAcceleratorPlan.h
  layout/ElementKind.cpp
  layout/LayoutCatalog.cpp
  layout/LayoutDialect.cpp
  layout/PlacementPlan.cpp
  layout/ShellDocument.cpp
  layout/ShellStatePolicy.cpp
  layout/StyleLookup.cpp
  layout/ThemeSurface.cpp
  layout/component/container/ContainerRegistry.cpp
  layout/component/generic/GenericRegistry.cpp
  layout/component/playback/NowPlayingInfo.cpp
  layout/component/playback/NowPlayingInfo.h
  layout/component/playback/PlaybackRegistry.cpp
  layout/component/playback/SoulButton.cpp
  layout/component/playback/SoulButton.h
  layout/component/shell/NavigationPane.cpp
  layout/component/shell/NavigationPane.h
  layout/component/shell/PaneSplitter.cpp
  layout/component/shell/PaneSplitter.h
  layout/component/shell/ShellRegistry.cpp
  layout/component/status/StatusRegistry.cpp
  layout/component/track/TrackCoverArt.cpp
  layout/component/track/TrackCoverArt.h
  layout/component/track/TrackDetail.cpp
  layout/component/track/TrackDetail.h
  layout/component/track/TrackRegistry.cpp
  layout/component/track/TrackTable.cpp
  layout/component/track/TrackTable.h
  layout/runtime/ActionBinder.cpp
  layout/runtime/ActionBinder.h
  layout/runtime/ActionRegistry.cpp
  layout/runtime/ActionRegistry.h
  layout/runtime/CommonLayoutProps.cpp
  layout/runtime/CommonLayoutProps.h
  layout/runtime/ComponentRegistrations.h
  layout/runtime/ComponentRegistry.cpp
  layout/runtime/ComponentRegistry.h
  layout/runtime/FocusedDetail.cpp
  layout/runtime/FocusedDetail.h
  layout/runtime/LayoutBuildContext.h
  layout/runtime/LayoutComponent.h
  layout/runtime/LayoutHost.cpp
  layout/runtime/LayoutHost.h
  layout/runtime/ResourceLookup.cpp
  layout/runtime/ResourceLookup.h
  layout/runtime/ShellLibraryAccess.h
  layout/runtime/UiSubscription.h
  layout/ShellBuilder.cpp
  layout/ShellBuilder.h
  layout/ShellPresetSource.cpp
  layout/ShellPresetSource.h
  platform/ScopedBooleanFlag.h
  platform/ProcessLauncher.cpp
  platform/ProcessLauncher.h
  platform/SmtcBridge.cpp
  platform/SmtcBridge.h
  platform/StringResources.cpp
  platform/StringResources.h
  platform/WinUiErrorBoundary.cpp
  include/ao/winui/WinUiErrorBoundary.h
  playback/AobusSoulControl.cpp
  playback/AobusSoulControl.h
  playback/AudioPipelineToolTip.cpp
  playback/AudioPipelineToolTip.h
  playback/MainWindowPlayback.cpp
  playback/OutputDeviceControl.cpp
  playback/OutputDeviceControl.h
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
  shell/MainWindowShell.cpp
  status/ActivityStatusControl.cpp
  status/ActivityStatusControl.h
  theme/Theme.cpp
  theme/SurfaceBrushes.cpp
  theme/SurfaceBrushes.h
  theme/ThemeCoordinator.cpp
  theme/ThemeCoordinator.h
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
  MainWindow.xaml
  MainWindow.xaml.cpp
  MainWindow.xaml.h
  Project.idl
  pch.h
)

add_library(aobus-winui-lib STATIC ${AOBUS_WINUI_LIBRARY_SOURCES})
target_precompile_headers(aobus-winui-lib PRIVATE pch.h)
target_compile_options(aobus-winui-lib PRIVATE /bigobj)
if(MSVC AND MSVC_VERSION EQUAL 1951)
  # MSVC 14.51 ICEs in Boost.Asio's any_io_executor machinery when this
  # translation unit is compiled with /GL. Keep IPO enabled for the rest of
  # the Release graph. The source must also skip the /GL-built PCH to avoid
  # C4652; remove both overrides after the toolset is fixed.
  set_property(SOURCE app/UiCoordinator.cpp APPEND PROPERTY
    COMPILE_OPTIONS "$<$<CONFIG:Release>:/GL->")
  set_property(SOURCE app/UiCoordinator.cpp PROPERTY
    SKIP_PRECOMPILE_HEADERS ON)
endif()
target_include_directories(aobus-winui-lib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aobus-winui-lib
  PUBLIC
    ao_app_desktop
    ao_app_runtime
    ao_app_uimodel
    ao_windows_platform
    PkgRapidYaml
  PRIVATE
    ao_app_i18n_ordering
    ao_app_i18n
    runtimeobject.lib
    shell32.lib)
add_dependencies(aobus-winui-lib
  ao_frontend_library_path_boundary_check
  ao_windows_winui_core_boundary_report
  ao_windows_winui_event_revoker_guardrail
  ao_windows_winui_leaf_capability_guardrail
  ao_windows_winui_state_subscription_guardrail)

add_executable(aobus-winui WIN32
  ${AOBUS_NO_COVER_ASSETS}
  ${AOBUS_SHELL_LAYOUT_ASSETS}
  ${AOBUS_CATALOG_WINUI_RESW_FILES}
  app.manifest
)

add_executable(ao_winui_localization_probe EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/test/helper/WinUiLocalizationProbe.cpp
  platform/StringResources.cpp
  ${AOBUS_CATALOG_WINUI_RESW_FILES}
)
aobus_enable_windows_utf8_manifest(ao_winui_localization_probe)
target_include_directories(ao_winui_localization_probe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ao_winui_localization_probe PRIVATE
  ao_app_i18n
  runtimeobject.lib)

aobus_verify_windows_utf8_manifest(aobus-winui)
target_include_directories(aobus-winui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aobus-winui PRIVATE
  aobus-winui-lib
  runtimeobject.lib
  shell32.lib)
# App.xaml generates wWinMain inside the static archive. Select the Unicode CRT
# entry explicitly so the linker extracts that object without /WHOLEARCHIVE,
# which would also extract and duplicate generated XAML metadata providers.
target_link_options(aobus-winui PRIVATE /ENTRY:wWinMainCRTStartup)
add_dependencies(aobus-winui ao_winui_localization_probe)
# The static-library project emits the XBF files at its output root, while
# MSBuild's project-reference copy namespaces them under Aobus. The merged
# application PRI retains the root-relative XBF paths, so deploy those exact
# candidates beside the executable when the projects use separate outputs.
add_custom_command(TARGET aobus-winui POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/App.xbf"
          "$<TARGET_FILE_DIR:aobus-winui>/App.xbf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/MainWindow.xbf"
          "$<TARGET_FILE_DIR:aobus-winui>/MainWindow.xbf"
  COMMAND $<TARGET_FILE:ao_winui_localization_probe>
  COMMENT "Deploying compiled XAML and verifying WinUI/ICU localization parity"
  VERBATIM)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  ${AOBUS_WINUI_LIBRARY_SOURCES}
  app.manifest)
source_group("Generated Localization" FILES
  ${AOBUS_CATALOG_WINUI_RESW_FILES})
source_group("Asset" FILES ${AOBUS_NO_COVER_ASSETS} ${AOBUS_SHELL_LAYOUT_ASSETS})

set_source_files_properties(App.xaml PROPERTIES
  VS_XAML_TYPE ApplicationDefinition
  VS_SETTINGS "Link=App.xaml")
set_source_files_properties(MainWindow.xaml PROPERTIES
  VS_TOOL_OVERRIDE Page
  VS_SETTINGS "Link=MainWindow.xaml")
set_source_files_properties(Project.idl PROPERTIES
  VS_TOOL_OVERRIDE Midl
  VS_SETTINGS "SubType=Code;OutputDirectory=$(ProjectDir)")
set_source_files_properties(${AOBUS_CATALOG_EN_RESW} PROPERTIES
  GENERATED TRUE
  VS_TOOL_OVERRIDE PRIResource
  VS_SETTINGS "Link=Resources\\en\\Resources.resw")
set_source_files_properties(${AOBUS_CATALOG_DE_RESW} PROPERTIES
  GENERATED TRUE
  VS_TOOL_OVERRIDE PRIResource
  VS_SETTINGS "Link=Resources\\de\\Resources.resw")
set_source_files_properties(${AOBUS_CATALOG_ZH_HANS_RESW} PROPERTIES
  GENERATED TRUE
  VS_TOOL_OVERRIDE PRIResource
  VS_SETTINGS "Link=Resources\\zh-Hans\\Resources.resw")
set_source_files_properties(${AOBUS_CATALOG_ZH_HANT_RESW} PROPERTIES
  GENERATED TRUE
  VS_TOOL_OVERRIDE PRIResource
  VS_SETTINGS "Link=Resources\\zh-Hant\\Resources.resw")
set_source_files_properties(${AOBUS_CATALOG_JA_RESW} PROPERTIES
  GENERATED TRUE
  VS_TOOL_OVERRIDE PRIResource
  VS_SETTINGS "Link=Resources\\ja\\Resources.resw")
set_source_files_properties(${AOBUS_CATALOG_ES_RESW} PROPERTIES
  GENERATED TRUE
  VS_TOOL_OVERRIDE PRIResource
  VS_SETTINGS "Link=Resources\\es\\Resources.resw")
set_source_files_properties(${AOBUS_CATALOG_FR_RESW} PROPERTIES
  GENERATED TRUE
  VS_TOOL_OVERRIDE PRIResource
  VS_SETTINGS "Link=Resources\\fr\\Resources.resw")
set_source_files_properties(${AOBUS_CATALOG_PSEUDO_RESW} PROPERTIES
  GENERATED TRUE
  VS_TOOL_OVERRIDE PRIResource
  VS_SETTINGS "Link=Resources\\qps-ploc\\Resources.resw")
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
set_source_files_properties(${AOBUS_MODERN_LAYOUT_ASSET} PROPERTIES
  VS_TOOL_OVERRIDE Content
  VS_COPY_TO_OUT_DIR PreserveNewest
  VS_SETTINGS "Link=Assets\\Layout\\windows_modern_layout.yaml")
set_source_files_properties(${AOBUS_CLASSIC_LAYOUT_ASSET} PROPERTIES
  VS_TOOL_OVERRIDE Content
  VS_COPY_TO_OUT_DIR PreserveNewest
  VS_SETTINGS "Link=Assets\\Layout\\windows_classic_layout.yaml")
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

set_target_properties(aobus-winui PROPERTIES
  OUTPUT_NAME Aobus
  VS_GLOBAL_AppContainerApplication false
  VS_GLOBAL_ApplicationType "Windows Store"
  VS_GLOBAL_ApplicationTypeRevision 10.0
  VS_GLOBAL_DefaultLanguage en
  VS_GLOBAL_DesktopCompatible true
  VS_GLOBAL_MinimumVisualStudioVersion 18.0
  VS_GLOBAL_RootNamespace Aobus
  VS_GLOBAL_TargetName Aobus
  VS_GLOBAL_WindowsTargetPlatformMinVersion 10.0.26100.0
  WindowsTargetPlatformVersion 10.0.26100.0)
# MakePri treats copy-local PRI files from referenced projects as merge inputs.
# Dedicated output directories keep a previous Aobus.pri from being
# rediscovered through a referenced target during an incremental catalog build.
set_target_properties(ao_winui_localization_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/localization-probe"
  VS_GLOBAL_AppContainerApplication false
  VS_GLOBAL_ApplicationType "Windows Store"
  VS_GLOBAL_ApplicationTypeRevision 10.0
  VS_GLOBAL_DefaultLanguage en
  VS_GLOBAL_DesktopCompatible true
  VS_GLOBAL_MinimumVisualStudioVersion 18.0
  VS_GLOBAL_RootNamespace AobusLocalizationProbe
  VS_GLOBAL_TargetName AobusLocalizationProbe
  VS_GLOBAL_WindowsTargetPlatformMinVersion 10.0.26100.0
  WindowsTargetPlatformVersion 10.0.26100.0)
set_target_properties(aobus-winui-lib PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/winui-lib"
  VS_GLOBAL_AppContainerApplication false
  VS_GLOBAL_ApplicationType "Windows Store"
  VS_GLOBAL_ApplicationTypeRevision 10.0
  VS_GLOBAL_DesktopCompatible true
  VS_GLOBAL_MinimumVisualStudioVersion 18.0
  VS_GLOBAL_RootNamespace Aobus
  VS_GLOBAL_WindowsTargetPlatformMinVersion 10.0.26100.0
  WindowsTargetPlatformVersion 10.0.26100.0)

aobus_enable_windows_app_sdk(aobus-winui-lib STATIC_LIBRARY)
aobus_enable_windows_app_sdk(aobus-winui APPLICATION)
aobus_enable_windows_app_sdk(ao_winui_localization_probe APPLICATION)
