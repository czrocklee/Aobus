// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ao::utility
{
  // These operations are for user-visible text. Filesystem paths remain native
  // path values and must use the explicit conversions in Path.h instead.
  Result<> validateUtf8(std::string_view text);

  // Validates the complete input before reporting whether it is already NFC.
  Result<bool> isUtf8Nfc(std::string_view text);

  Result<std::string> normalizeUtf8Nfc(std::string_view text);

  Result<std::string> makeUtf8CaselessKey(std::string_view text);

  // Validates the complete text and byte offset before checking a grapheme
  // boundary. Both ends are boundaries; offsets inside a scalar or cluster are not.
  Result<bool> isUtf8GraphemeBoundary(std::string_view text, std::size_t offset);

  // Returns the UTF-8 byte offset of the extended grapheme boundary immediately
  // before @p offset, or zero when no earlier boundary exists. An offset past
  // the end of @p text is rejected; an offset inside a cluster resolves to that
  // cluster's start.
  Result<std::size_t> previousUtf8GraphemeBoundary(std::string_view text, std::size_t offset);

  // Returns the UTF-8 byte offset of the extended grapheme boundary immediately
  // after @p offset, or the size of @p text when no later boundary exists. The
  // offset rules of the previous-boundary operation apply, except that an
  // offset inside a cluster resolves to that cluster's end.
  Result<std::size_t> nextUtf8GraphemeBoundary(std::string_view text, std::size_t offset);
} // namespace ao::utility
