# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Guards against forbidden includes creeping into shared library directories.
#
# Usage:
#   cmake
#   -DROOTS="<semicolon-separated list of source directories>"
#   -DFORBIDDEN_REGEX="<regex>"
#   -DEXCLUDE_REGEX="<regex>"
#   -DMODE="<report|enforce>"
#   -P cmake/AssertNoForbiddenIncludes.cmake
#
# Exits with FATAL_ERROR in enforce mode if any non-excluded file under ROOTS
# matches FORBIDDEN_REGEX. Report mode prints the same findings as warnings.

if(NOT ROOTS)
  message(FATAL_ERROR "AssertNoForbiddenIncludes: ROOTS not specified")
endif()

set(_mode "enforce")
if(MODE)
  set(_mode "${MODE}")
endif()

if(NOT _mode STREQUAL "enforce" AND NOT _mode STREQUAL "report")
  message(FATAL_ERROR "AssertNoForbiddenIncludes: MODE must be 'enforce' or 'report' (got '${_mode}')")
endif()

set(_regex "#[ \t]*include[ \t]*[<\"]\(gtkmm|gdkmm|giomm|glibmm|gtk|gdk|gio|glib)/")

if(FORBIDDEN_REGEX)
  set(_regex "${FORBIDDEN_REGEX}")
endif()

set(_findings "")
set(_finding_count 0)

foreach(root IN LISTS ROOTS)
  file(GLOB_RECURSE sources "${root}/*.h" "${root}/*.hpp" "${root}/*.cpp")

  foreach(source IN LISTS sources)
    if(EXCLUDE_REGEX AND source MATCHES "${EXCLUDE_REGEX}")
      continue()
    endif()

    file(READ "${source}" content)

    if(content MATCHES "${_regex}")
      # Extract the matched line for a helpful message
      string(REGEX MATCH "${_regex}" matched_line "${content}")
      set(_finding "Forbidden include in ${source}: '${matched_line}'")
      list(APPEND _findings "${_finding}")
      math(EXPR _finding_count "${_finding_count} + 1")

      if(_mode STREQUAL "report")
        message(WARNING "${_finding}")
      endif()
    endif()
  endforeach()
endforeach()

if(_finding_count GREATER 0 AND _mode STREQUAL "enforce")
  list(JOIN _findings "\n  " _finding_text)
  message(FATAL_ERROR
    "AssertNoForbiddenIncludes: found ${_finding_count} forbidden include(s):\n  ${_finding_text}")
endif()

if(_finding_count GREATER 0 AND _mode STREQUAL "report")
  message(WARNING "AssertNoForbiddenIncludes: report mode found ${_finding_count} forbidden include(s)")
endif()
