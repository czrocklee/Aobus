# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2025 Aobus Contributors
#
# Validation module for the native WinUI frontend.

# Validation owns probe builds, deployment and unattended execution. Keep this
# utility aggregate out of the product's dependencies: application references
# to probes would import their WinMD/PRI closure into the shipping executable.
add_custom_target(aobus-winui-check
  # The Soul probe parses the same shipping App.xaml, so it needs the
  # root-relative XBF and the merged application PRI that maps the ms-appx
  # locator. The aggregate waits for the application and every probe first.
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/App.xbf"
          "$<TARGET_FILE_DIR:ao_winui_soul_probe>/App.xbf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui>/Aobus.pri"
          "$<TARGET_FILE_DIR:ao_winui_soul_probe>/ao_winui_soul_probe.pri"
  # The track table probe loads the shipping frame's compiled header template
  # and resolves MRT resources, so it takes both root-relative XBFs and a PRI
  # named after its own executable. It runs manually on a desktop, not here.
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/App.xbf"
          "$<TARGET_FILE_DIR:ao_winui_track_table_probe>/App.xbf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/MainWindow.xbf"
          "$<TARGET_FILE_DIR:ao_winui_track_table_probe>/MainWindow.xbf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui>/Aobus.pri"
          "$<TARGET_FILE_DIR:ao_winui_track_table_probe>/ao_winui_track_table_probe.pri"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/App.xbf"
          "$<TARGET_FILE_DIR:ao_winui_activity_status_probe>/App.xbf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/MainWindow.xbf"
          "$<TARGET_FILE_DIR:ao_winui_activity_status_probe>/MainWindow.xbf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui>/Aobus.pri"
          "$<TARGET_FILE_DIR:ao_winui_activity_status_probe>/ao_winui_activity_status_probe.pri"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/App.xbf"
          "$<TARGET_FILE_DIR:ao_winui_automation_probe>/App.xbf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/MainWindow.xbf"
          "$<TARGET_FILE_DIR:ao_winui_automation_probe>/MainWindow.xbf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui>/Aobus.pri"
          "$<TARGET_FILE_DIR:ao_winui_automation_probe>/ao_winui_automation_probe.pri"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/App.xbf"
          "$<TARGET_FILE_DIR:ao_winui_credits_probe>/App.xbf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui>/Aobus.pri"
          "$<TARGET_FILE_DIR:ao_winui_credits_probe>/ao_winui_credits_probe.pri"
  # The quality tooltip probe uses the same shipping application resources.
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui-lib>/App.xbf"
          "$<TARGET_FILE_DIR:ao_winui_quality_tooltip_probe>/App.xbf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:aobus-winui>/Aobus.pri"
          "$<TARGET_FILE_DIR:ao_winui_quality_tooltip_probe>/ao_winui_quality_tooltip_probe.pri"
  COMMAND $<TARGET_FILE:ao_winui_track_item_probe>
  COMMAND $<TARGET_FILE:ao_winui_localization_probe>
  DEPENDS
    aobus-winui
    ao_winui_localization_probe
    ao_winui_track_item_probe
    ao_winui_track_table_probe
    ao_winui_activity_status_probe
    ao_winui_automation_probe
    ao_winui_soul_probe
    ao_winui_credits_probe
    ao_winui_quality_tooltip_probe
  COMMENT "Deploying probe resources and verifying native WinUI contracts"
  VERBATIM)
