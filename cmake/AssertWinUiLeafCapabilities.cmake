# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Keeps high-authority window/session objects at WinUI composition roots.

if(NOT ROOT)
  message(FATAL_ERROR "AssertWinUiLeafCapabilities: ROOT not specified")
endif()

set(_ao_files)
foreach(_subdir IN ITEMS layout/component layout/runtime playback status track image)
  file(GLOB_RECURSE _ao_subdir_files LIST_DIRECTORIES false
       "${ROOT}/${_subdir}/*.h"
       "${ROOT}/${_subdir}/*.cpp")
  list(APPEND _ao_files ${_ao_subdir_files})
endforeach()

# These files are the explicit composition/coordinator roots for the scanned
# subtrees. Everything else is a generation component or leaf adapter and must
# receive exact capabilities.
set(_ao_composition_root_files
    "${ROOT}/layout/ShellBuilder.h"
    "${ROOT}/layout/ShellBuilder.cpp"
    "${ROOT}/playback/MainWindowPlayback.cpp"
    "${ROOT}/track/MainWindowTrack.cpp"
    "${ROOT}/track/TrackListController.h"
    "${ROOT}/track/TrackListController.cpp")
list(REMOVE_ITEM _ao_files ${_ao_composition_root_files})
list(REMOVE_DUPLICATES _ao_files)
list(SORT _ao_files)

foreach(_file IN LISTS _ao_files)
  file(RELATIVE_PATH _rel "${ROOT}" "${_file}")
  file(STRINGS "${_file}" _matches REGEX "WinUiDependencies|LibrarySession|AppRuntime")

  foreach(_line IN LISTS _matches)
    string(REGEX REPLACE "//.*$" "" _code "${_line}")

    if(_code MATCHES "WinUiDependencies|LibrarySession|AppRuntime")
      message(FATAL_ERROR
              "AssertWinUiLeafCapabilities: ${_rel} reaches a high-authority dependency: ${_line}\n"
              "  Keep session/runtime composition at the window or ShellBuilder and pass the exact service or callback.")
    endif()
  endforeach()
endforeach()
