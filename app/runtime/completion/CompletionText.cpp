// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/completion/CompletionText.h>

#include <ao/rt/completion/CompletionAliasPolicy.h>
#include <ao/utility/String.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  namespace
  {
    constexpr unsigned char kAsciiMax = 0x7fU;
  }

  bool startsWithCompletionPrefixInsensitive(std::string_view candidate, std::string_view prefix)
  {
    if (prefix.size() > candidate.size())
    {
      return false;
    }

    return std::equal(prefix.begin(),
                      prefix.end(),
                      candidate.begin(),
                      [](char lhs, char rhs) { return completionLowerAscii(lhs) == completionLowerAscii(rhs); });
  }

  std::optional<std::size_t> findCompletionWordPrefixInsensitive(std::string_view const candidate,
                                                                 std::string_view const prefix)
  {
    if (prefix.size() > candidate.size())
    {
      return std::nullopt;
    }

    auto const lastOffset = candidate.size() - prefix.size();

    for (std::size_t offset = 0; offset <= lastOffset; ++offset)
    {
      if (offset != 0)
      {
        auto const previous = static_cast<unsigned char>(candidate[offset - 1]);

        if (previous > kAsciiMax || utility::isAsciiAlphaNumeric(candidate[offset - 1]))
        {
          continue;
        }
      }

      if (startsWithCompletionPrefixInsensitive(candidate.substr(offset), prefix))
      {
        return offset;
      }
    }

    return std::nullopt;
  }

  std::optional<std::string> makeCompletionAliasPrefixKey(std::string_view const prefix)
  {
    auto key = std::string{};
    key.reserve(prefix.size());

    for (auto const ch : prefix)
    {
      if (static_cast<unsigned char>(ch) > kAsciiMax)
      {
        return std::nullopt;
      }

      if (utility::isAsciiAlpha(ch))
      {
        key.push_back(utility::toAsciiLower(ch));
      }
      else if (utility::isAsciiDigit(ch))
      {
        key.push_back(ch);
      }
    }

    if (key.size() < kMinimumCompletionAliasLength)
    {
      return std::nullopt;
    }

    return key;
  }
} // namespace ao::rt
