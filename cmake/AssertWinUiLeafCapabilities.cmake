# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Keeps high-authority window/session objects at WinUI composition roots.

if(NOT ROOT)
  message(FATAL_ERROR "AssertWinUiLeafCapabilities: ROOT not specified")
endif()

file(GLOB_RECURSE _ao_component_files LIST_DIRECTORIES false
     "${ROOT}/layout/component/*.h"
     "${ROOT}/layout/component/*.cpp")

set(_ao_files
    ${_ao_component_files}
    "${ROOT}/layout/runtime/LayoutBuildContext.h"
    "${ROOT}/playback/AudioPipelineToolTip.h"
    "${ROOT}/playback/AudioPipelineToolTip.cpp"
    "${ROOT}/playback/OutputDeviceControl.h"
    "${ROOT}/playback/OutputDeviceControl.cpp"
    "${ROOT}/playback/PlaybackTimeControl.h"
    "${ROOT}/playback/PlaybackTimeControl.cpp"
    "${ROOT}/playback/SeekControl.h"
    "${ROOT}/playback/SeekControl.cpp"
    "${ROOT}/playback/SoulTransportButton.h"
    "${ROOT}/playback/SoulTransportButton.cpp"
    "${ROOT}/playback/TransportButton.h"
    "${ROOT}/playback/TransportButton.cpp"
    "${ROOT}/playback/VolumeControl.h"
    "${ROOT}/playback/VolumeControl.cpp"
    "${ROOT}/status/ActivityStatusControl.h"
    "${ROOT}/status/ActivityStatusControl.cpp"
    "${ROOT}/track/TrackQuickFilterControl.h"
    "${ROOT}/track/TrackQuickFilterControl.cpp")

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
