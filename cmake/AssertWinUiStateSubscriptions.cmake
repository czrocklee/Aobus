# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Keeps WinUI state delivery component-scoped.
#
# Components read an observable source's current value and retain the returned
# ao::async::Subscription. Reintroducing virtual callbacks or a raw observer
# vector would split registration from component lifetime and make generation
# publication responsible for synchronizing a second object graph.

if(NOT ROOT)
  message(FATAL_ERROR "AssertWinUiStateSubscriptions: ROOT not specified")
endif()

set(_ao_forbidden_identifiers
    "onShellStateChanged"
    "onTrackListChanged"
    "onWindowActivityChanged"
    "onStatusMessageChanged")

file(GLOB_RECURSE _ao_files LIST_DIRECTORIES false
     "${ROOT}/*.h"
     "${ROOT}/*.hpp"
     "${ROOT}/*.cpp")

foreach(_file IN LISTS _ao_files)
  file(RELATIVE_PATH _rel "${ROOT}" "${_file}")

  foreach(_identifier IN LISTS _ao_forbidden_identifiers)
    file(STRINGS "${_file}" _matches REGEX "${_identifier}")

    foreach(_line IN LISTS _matches)
      message(FATAL_ERROR
              "AssertWinUiStateSubscriptions: ${_rel} restores the old '${_identifier}' push contract: ${_line}\n"
              "  Read the source's current value, connect to its ao::async::Signal,"
              " and retain the Subscription in the component instead.")
    endforeach()
  endforeach()

  file(STRINGS "${_file}" _observer_pushes
       REGEX "(ctx|context|generation)[.]observers|observers[.]push_back")

  foreach(_line IN LISTS _observer_pushes)
    message(FATAL_ERROR
            "AssertWinUiStateSubscriptions: ${_rel} restores raw component fan-out: ${_line}\n"
            "  The layout host owns generations only; state sources own change delivery.")
  endforeach()
endforeach()
