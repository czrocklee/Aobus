# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors
#
# Comment-free source views for the architecture guards.
#
# A guard decides its rule on code, never on prose. Comments here routinely name
# the very types the guards forbid - a leaf documents that it takes exact
# capabilities rather than the whole AppRuntime - and a block comment spans
# lines, so no line-oriented test can tell the two apart. Treating a line that
# opens with `*` as commentary reports that doc comment; keeping it hides code
# that shares the line with the terminator. Both scans below track the comment
# state across lines instead, so each guard states its rule and nothing else.

# Appends the code half of _ao_line to _ao_code, carrying _ao_in_comment across
# calls. Each comment collapses to one space, so a comment cannot join the
# tokens on either side of it.
macro(_ao_strip_comments_from_line)
  set(_ao_rest "${_ao_line}")
  set(_ao_code "")

  while(NOT _ao_rest STREQUAL "")
    if(_ao_in_comment)
      string(FIND "${_ao_rest}" "*/" _ao_at)

      if(_ao_at EQUAL -1)
        set(_ao_rest "")
      else()
        math(EXPR _ao_at "${_ao_at} + 2")
        string(SUBSTRING "${_ao_rest}" ${_ao_at} -1 _ao_rest)
        string(APPEND _ao_code " ")
        set(_ao_in_comment FALSE)
      endif()
    else()
      string(FIND "${_ao_rest}" "/*" _ao_block)
      string(FIND "${_ao_rest}" "//" _ao_eol)

      if(_ao_eol GREATER -1 AND (_ao_block EQUAL -1 OR _ao_eol LESS _ao_block))
        string(SUBSTRING "${_ao_rest}" 0 ${_ao_eol} _ao_kept)
        string(APPEND _ao_code "${_ao_kept} ")
        set(_ao_rest "")
      elseif(_ao_block EQUAL -1)
        string(APPEND _ao_code "${_ao_rest}")
        set(_ao_rest "")
      else()
        string(SUBSTRING "${_ao_rest}" 0 ${_ao_block} _ao_kept)
        string(APPEND _ao_code "${_ao_kept}")
        math(EXPR _ao_block "${_ao_block} + 2")
        string(SUBSTRING "${_ao_rest}" ${_ao_block} -1 _ao_rest)
        set(_ao_in_comment TRUE)
      endif()
    endif()
  endwhile()
endmacro()

# ao_find_code_line(<out> <file> <regex>)
#
# Sets <out> to the first line of <file> whose code matches <regex>, or to the
# empty string when no line does. The line is reported as written, so a guard
# can quote the source rather than its own stripped view of it.
function(ao_find_code_line out_var file regex)
  set(_ao_in_comment FALSE)
  file(STRINGS "${file}" _ao_lines)

  foreach(_ao_line IN LISTS _ao_lines)
    _ao_strip_comments_from_line()

    if(_ao_code MATCHES "${regex}")
      set("${out_var}" "${_ao_line}" PARENT_SCOPE)
      return()
    endif()
  endforeach()

  set("${out_var}" "" PARENT_SCOPE)
endfunction()

# ao_read_code_text(<out> <file>)
#
# Sets <out> to the code of <file> with every run of whitespace collapsed to one
# space, so a construct that wraps across lines reads the same as its inline
# spelling.
function(ao_read_code_text out_var file)
  set(_ao_in_comment FALSE)
  set(_ao_text "")
  file(STRINGS "${file}" _ao_lines)

  foreach(_ao_line IN LISTS _ao_lines)
    _ao_strip_comments_from_line()
    string(APPEND _ao_text "${_ao_code} ")
  endforeach()

  string(REGEX REPLACE "[ \t\r\n]+" " " _ao_text "${_ao_text}")
  set("${out_var}" "${_ao_text}" PARENT_SCOPE)
endfunction()
