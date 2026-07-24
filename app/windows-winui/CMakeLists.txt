if(NOT WIN32)
  message(FATAL_ERROR "The WinUI frontend is available only on native Windows.")
endif()

add_executable(aobus-winui WIN32
  App.xaml
  App.xaml.cpp
  App.xaml.h
  MainWindow.xaml
  MainWindow.xaml.cpp
  MainWindow.xaml.h
  Project.idl
  app.manifest
  pch.h
)

set_source_files_properties(App.xaml PROPERTIES
  VS_XAML_TYPE ApplicationDefinition)
set_source_files_properties(MainWindow.xaml PROPERTIES
  VS_TOOL_OVERRIDE Page)
set_source_files_properties(Project.idl PROPERTIES
  VS_TOOL_OVERRIDE Midl
  VS_SETTINGS "SubType=Code;OutputDirectory=$(ProjectDir)")
set_source_files_properties(app.manifest PROPERTIES
  VS_TOOL_OVERRIDE Manifest)
set_source_files_properties(App.xaml.cpp PROPERTIES
  VS_SETTINGS "DependentUpon=App.xaml")
set_source_files_properties(App.xaml.h PROPERTIES
  VS_SETTINGS "DependentUpon=App.xaml")
set_source_files_properties(MainWindow.xaml.cpp PROPERTIES
  VS_SETTINGS "DependentUpon=MainWindow.xaml")
set_source_files_properties(MainWindow.xaml.h PROPERTIES
  VS_SETTINGS "DependentUpon=MainWindow.xaml")

target_precompile_headers(aobus-winui PRIVATE pch.h)
target_compile_options(aobus-winui PRIVATE /bigobj)
target_include_directories(aobus-winui PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aobus-winui PRIVATE
  ao_app_runtime
  ao_app_uimodel
  runtimeobject.lib)

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
