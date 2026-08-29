# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors
#
# Keeps AppRuntime at GTK layout composition roots.

if(NOT ROOT)
  message(FATAL_ERROR "AssertGtkLeafCapabilities: ROOT not specified")
endif()

file(GLOB_RECURSE _ao_files LIST_DIRECTORIES false
     "${ROOT}/layout/component/*.h"
     "${ROOT}/layout/component/*.cpp")

# Group registrations are the composition roots that unpack AppRuntime into
# exact services for the component factories beside them.
list(FILTER _ao_files EXCLUDE REGEX "/ComponentRegistrations[.]h$")
list(FILTER _ao_files EXCLUDE REGEX "/[A-Za-z]+Registrations[.]cpp$")
list(SORT _ao_files)

foreach(_file IN LISTS _ao_files)
  file(RELATIVE_PATH _rel "${ROOT}" "${_file}")
  file(STRINGS "${_file}" _matches REGEX "GtkUiDependencies|AppRuntime")

  foreach(_line IN LISTS _matches)
    string(REGEX REPLACE "//.*$" "" _code "${_line}")

    if(_code MATCHES "GtkUiDependencies|AppRuntime")
      message(FATAL_ERROR
              "AssertGtkLeafCapabilities: ${_rel} reaches a high-authority dependency: ${_line}\n"
              "  Unpack the runtime in a *Registrations.cpp composition root and pass exact capabilities.")
    endif()
  endforeach()
endforeach()
