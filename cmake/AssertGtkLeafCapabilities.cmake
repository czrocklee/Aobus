# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors
#
# Keeps AppRuntime at GTK composition roots.

include("${CMAKE_CURRENT_LIST_DIR}/AoSourceCode.cmake")

if(NOT ROOT)
  message(FATAL_ERROR "AssertGtkLeafCapabilities: ROOT not specified")
endif()

file(GLOB_RECURSE _ao_files LIST_DIRECTORIES false
     "${ROOT}/*.h"
     "${ROOT}/*.hpp"
     "${ROOT}/*.cpp")

# The whole frontend is scanned, so the roots are named rather than matched by
# a filename shape. Group registrations unpack the runtime into exact services
# for the component factories beside them; the window, its shell layout, and
# the layout runtime compose those groups; the list navigation controller
# builds the smart-list dialog and forwards two capabilities it never uses
# itself. Everything else must receive exact capabilities.
set(_ao_composition_root_files
    "${ROOT}/app/LibraryWindowLifecycle.cpp"
    "${ROOT}/app/MainWindow.h"
    "${ROOT}/app/MainWindow.cpp"
    "${ROOT}/app/ShellLayoutController.h"
    "${ROOT}/app/ShellLayoutController.cpp"
    "${ROOT}/layout/component/ComponentRegistrations.h"
    "${ROOT}/layout/component/playback/PlaybackRegistrations.cpp"
    "${ROOT}/layout/component/status/StatusRegistrations.cpp"
    "${ROOT}/layout/component/track/TrackRegistrations.cpp"
    "${ROOT}/layout/runtime/LayoutRuntime.h"
    "${ROOT}/layout/runtime/LayoutRuntime.cpp"
    "${ROOT}/list/ListNavigationController.h"
    "${ROOT}/list/ListNavigationController.cpp")
list(REMOVE_ITEM _ao_files ${_ao_composition_root_files})
list(REMOVE_DUPLICATES _ao_files)
list(SORT _ao_files)

foreach(_file IN LISTS _ao_files)
  ao_find_code_line(_ao_reach "${_file}" "AppRuntime")

  if(NOT _ao_reach STREQUAL "")
    file(RELATIVE_PATH _rel "${ROOT}" "${_file}")
    message(FATAL_ERROR
            "AssertGtkLeafCapabilities: ${_rel} reaches a high-authority dependency: ${_ao_reach}\n"
            "  Unpack the runtime in a named composition root and pass exact capabilities.")
  endif()
endforeach()
