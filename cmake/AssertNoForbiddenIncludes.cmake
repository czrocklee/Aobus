# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Guards against forbidden dependencies creeping into shared library directories.
#
# Usage:
#   cmake
#   -DROOTS="<semicolon-separated list of source directories>"
#   -DFORBIDDEN_REGEX="<regex>"
#   -DFORBIDDEN_REGEX_FILE="<path>"
#   -DEXCLUDE_REGEX="<regex>"
#   -DEXCLUDE_REGEX_FILE="<path>"
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

if(FORBIDDEN_REGEX_FILE)
  file(READ "${FORBIDDEN_REGEX_FILE}" _regex)
elseif(FORBIDDEN_REGEX)
  set(_regex "${FORBIDDEN_REGEX}")
endif()

set(_exclude_regex "")
if(EXCLUDE_REGEX_FILE)
  file(READ "${EXCLUDE_REGEX_FILE}" _exclude_regex)
elseif(EXCLUDE_REGEX)
  set(_exclude_regex "${EXCLUDE_REGEX}")
endif()

set(_findings "")
set(_finding_count 0)

foreach(root IN LISTS ROOTS)
  if(IS_DIRECTORY "${root}")
    file(GLOB_RECURSE sources "${root}/*.h" "${root}/*.hpp" "${root}/*.cpp")
  elseif(EXISTS "${root}")
    set(sources "${root}")
  else()
    message(FATAL_ERROR "AssertNoForbiddenIncludes: root does not exist: ${root}")
  endif()

  foreach(source IN LISTS sources)
    if(_exclude_regex AND source MATCHES "${_exclude_regex}")
      continue()
    endif()

    file(READ "${source}" content)

    if(content MATCHES "${_regex}")
      # Extract the matched text for a helpful message.
      string(REGEX MATCH "${_regex}" matched_line "${content}")
      set(_finding "Forbidden dependency in ${source}: '${matched_line}'")
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
    "AssertNoForbiddenIncludes: found ${_finding_count} forbidden dependency match(es):\n  ${_finding_text}")
endif()

if(_finding_count GREATER 0 AND _mode STREQUAL "report")
  message(WARNING "AssertNoForbiddenIncludes: report mode found ${_finding_count} forbidden dependency match(es)")
endif()
