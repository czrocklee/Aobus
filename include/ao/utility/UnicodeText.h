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

  // Returns the UTF-8 byte offset immediately before the final extended
  // grapheme cluster, or zero for an empty string.
  Result<std::size_t> previousUtf8GraphemeBoundary(std::string_view text);
} // namespace ao::utility
