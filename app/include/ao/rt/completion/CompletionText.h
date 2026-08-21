// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/String.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  inline char completionLowerAscii(char ch)
  {
    return utility::toAsciiLower(ch);
  }

  bool startsWithCompletionPrefixInsensitive(std::string_view candidate, std::string_view prefix);

  /** Finds a case-insensitive prefix at the value start or after an ASCII word delimiter. */
  std::optional<std::size_t> findCompletionWordPrefixInsensitive(std::string_view candidate, std::string_view prefix);

  /** Compacts an ASCII-only typed prefix for transient alias lookup. */
  std::optional<std::string> makeCompletionAliasPrefixKey(std::string_view prefix);
} // namespace ao::rt
