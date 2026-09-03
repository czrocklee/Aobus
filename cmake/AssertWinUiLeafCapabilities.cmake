# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Keeps high-authority window/session objects at WinUI composition roots.

include("${CMAKE_CURRENT_LIST_DIR}/AoSourceCode.cmake")

if(NOT ROOT)
  message(FATAL_ERROR "AssertWinUiLeafCapabilities: ROOT not specified")
endif()

file(GLOB_RECURSE _ao_files LIST_DIRECTORIES false
     "${ROOT}/*.h"
     "${ROOT}/*.hpp"
     "${ROOT}/*.cpp")

# The whole frontend is scanned, so the roots are named rather than implied by
# which subdirectories the scan happens to cover. These files own the window,
# the session, and the shell composition; the window's implementation is split
# across three translation units that are roots for the same reason. Everything
# else is a generation component or leaf adapter and must receive exact
# capabilities.
set(_ao_composition_root_files
    "${ROOT}/MainWindow.xaml.h"
    "${ROOT}/MainWindow.xaml.cpp"
    "${ROOT}/app/LibrarySession.h"
    "${ROOT}/app/LibrarySession.cpp"
    "${ROOT}/app/LibraryWindowSession.h"
    "${ROOT}/app/LibraryWindowSession.cpp"
    "${ROOT}/layout/ShellBuilder.h"
    "${ROOT}/layout/ShellBuilder.cpp"
    "${ROOT}/playback/MainWindowPlayback.cpp"
    "${ROOT}/shell/MainWindowShell.cpp"
    "${ROOT}/track/MainWindowTrack.cpp")
list(REMOVE_ITEM _ao_files ${_ao_composition_root_files})
list(REMOVE_DUPLICATES _ao_files)
list(SORT _ao_files)

foreach(_file IN LISTS _ao_files)
  ao_find_code_line(_ao_reach "${_file}" "LibrarySession|AppRuntime")

  if(NOT _ao_reach STREQUAL "")
    file(RELATIVE_PATH _rel "${ROOT}" "${_file}")
    message(FATAL_ERROR
            "AssertWinUiLeafCapabilities: ${_rel} reaches a high-authority dependency: ${_ao_reach}\n"
            "  Keep session/runtime composition at the window or ShellBuilder and pass the exact service or callback.")
  endif()
endforeach()
