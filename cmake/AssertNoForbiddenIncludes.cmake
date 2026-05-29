# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Guards against forbidden includes creeping into shared library directories.
#
# Usage:
#   cmake
#   -DROOTS="<semicolon-separated list of source directories>"
#   -DFORBIDDEN_REGEX="<regex>"
#   -P cmake/AssertNoForbiddenIncludes.cmake
#
# Exits with FATAL_ERROR if any file under ROOTS matches FORBIDDEN_REGEX.

if(NOT ROOTS)
  message(FATAL_ERROR "AssertNoForbiddenIncludes: ROOTS not specified")
endif()

set(_regex "#[ \t]*include[ \t]*[<\"]\(gtkmm|gdkmm|giomm|glibmm|gtk|gdk|gio|glib)/")

if(FORBIDDEN_REGEX)
  set(_regex "${FORBIDDEN_REGEX}")
endif()

foreach(root IN LISTS ROOTS)
  file(GLOB_RECURSE sources "${root}/*.h" "${root}/*.hpp" "${root}/*.cpp")

  foreach(source IN LISTS sources)
    file(READ "${source}" content)

    if(content MATCHES "${_regex}")
      # Extract the matched line for a helpful message
      string(REGEX MATCH "${_regex}" matched_line "${content}")
      message(FATAL_ERROR
        "Forbidden include in ${source}: '${matched_line}'")
    endif()
  endforeach()
endforeach()
