# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors

foreach(_required IN ITEMS INPUT OUTPUT)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "EmbedBinary.cmake requires -D${_required}=...")
  endif()
endforeach()

if(NOT EXISTS "${INPUT}")
  message(FATAL_ERROR "EmbedBinary.cmake input does not exist: ${INPUT}")
endif()

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _hex_length)
if(_hex_length EQUAL 0)
  message(FATAL_ERROR "EmbedBinary.cmake refuses to embed an empty file: ${INPUT}")
endif()

get_filename_component(_output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_directory}")
file(WRITE "${OUTPUT}" [[// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors
// Generated from the governed ICU common-data package. Do not edit.

#include "app/i18n/EmbeddedCatalogData.h"

#include <cstddef>
#include <span>

namespace ao::i18n::detail
{
  namespace
  {
    alignas(16) constexpr std::byte kEmbeddedCatalogData[] = {
]])

math(EXPR _full_hex_length "${_hex_length} - (${_hex_length} % 16)")
set(_body "")

if(_full_hex_length GREATER 0)
  string(SUBSTRING "${_hex}" 0 ${_full_hex_length} _full_hex)
  string(REGEX REPLACE
    "([0-9A-Fa-f][0-9A-Fa-f])([0-9A-Fa-f][0-9A-Fa-f])([0-9A-Fa-f][0-9A-Fa-f])([0-9A-Fa-f][0-9A-Fa-f])([0-9A-Fa-f][0-9A-Fa-f])([0-9A-Fa-f][0-9A-Fa-f])([0-9A-Fa-f][0-9A-Fa-f])([0-9A-Fa-f][0-9A-Fa-f])"
    "      std::byte{0x\\1}, std::byte{0x\\2}, std::byte{0x\\3}, std::byte{0x\\4}, std::byte{0x\\5}, std::byte{0x\\6}, std::byte{0x\\7}, std::byte{0x\\8},\n"
    _body
    "${_full_hex}")
endif()

set(_tail "")
math(EXPR _tail_hex_length "${_hex_length} - ${_full_hex_length}")

if(_tail_hex_length GREATER 0)
  math(EXPR _tail_byte_count "${_tail_hex_length} / 2")
  math(EXPR _last_tail_byte "${_tail_byte_count} - 1")
  set(_tail "      ")

  foreach(_index RANGE 0 ${_last_tail_byte})
    math(EXPR _offset "${_full_hex_length} + (${_index} * 2)")
    string(SUBSTRING "${_hex}" ${_offset} 2 _byte)
    string(APPEND _tail "std::byte{0x${_byte}},")

    if(NOT _index EQUAL _last_tail_byte)
      string(APPEND _tail " ")
    endif()
  endforeach()

  string(APPEND _tail "\n")
endif()

file(APPEND "${OUTPUT}" "${_body}${_tail}")

file(APPEND "${OUTPUT}" [[    };
  } // namespace

  std::span<std::byte const> embeddedCatalogData() noexcept
  {
    return kEmbeddedCatalogData;
  }
} // namespace ao::i18n::detail
]])
