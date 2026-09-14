add_library(ao_appkit OBJECT
  ActivityExpirationTimer.mm ActivityExpirationTimer.h
  ActivityPopover.mm ActivityPopover.h
  AppKitText.mm AppKitText.h
  ArtworkView.mm ArtworkView.h
  DesktopApplication.mm DesktopApplication.h DesktopApplicationInternal.h
  DesktopShell.mm
  DesktopCommandSurface.mm
  DesktopControls.mm DesktopControls.h
  LibraryBrowser.mm LibraryBrowser.h
  LibraryEditor.mm LibraryEditor.h
  LibraryEditorModel.cpp LibraryEditorModel.h
  LibrarySession.cpp LibrarySession.h
  MainRunLoopExecutor.cpp MainRunLoopExecutor.h
  PlaybackBar.mm PlaybackBar.h
  SoulButton.mm SoulButton.h
  TrackInspector.mm TrackInspector.h)
target_link_libraries(ao_appkit PUBLIC
  ao_app_uimodel ao_app_runtime ao_app_i18n ao_app_i18n_ordering ao_audio ao_desktop_launch
  "-framework AppKit" "-framework CoreFoundation" "-framework QuartzCore")
target_compile_options(ao_appkit PUBLIC
  "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc;-Woverloaded-virtual;-Wnon-virtual-dtor>")
if(AOBUS_ENABLE_ASAN)
  target_compile_options(ao_appkit PUBLIC
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fsanitize=address,undefined;-fno-omit-frame-pointer>")
elseif(AOBUS_ENABLE_TSAN)
  target_compile_options(ao_appkit PUBLIC
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fsanitize=thread;-fno-omit-frame-pointer>")
endif()

set_target_properties(ao_appkit PROPERTIES
  OBJCXX_STANDARD 26 OBJCXX_STANDARD_REQUIRED ON)

# The shipping bundle and native GUI tests share these exact production objects.
add_executable(aobus-appkit MACOSX_BUNDLE DesktopMain.mm)
target_link_libraries(aobus-appkit PRIVATE ao_appkit)
set(_appkit_icon "${CMAKE_SOURCE_DIR}/asset/brand/Aobus.icns")
set(_appkit_brand_license "${CMAKE_SOURCE_DIR}/asset/brand/LICENSE.txt")
target_sources(aobus-appkit PRIVATE "${_appkit_icon}" "${_appkit_brand_license}")
set_source_files_properties("${_appkit_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_source_files_properties("${_appkit_brand_license}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Brand)
set_target_properties(aobus-appkit PROPERTIES
  OBJCXX_STANDARD 26 OBJCXX_STANDARD_REQUIRED ON
  MACOSX_BUNDLE_GUI_IDENTIFIER "org.aobus.desktop"
  MACOSX_BUNDLE_BUNDLE_NAME "Aobus"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/DesktopInfo.plist.in")
