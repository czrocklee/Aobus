// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Internal vocabulary matching and rendering helpers for ao_app_runtime completion code.

#include <ao/rt/CompletionItem.h>
#include <ao/rt/CompletionService.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  inline char completionLowerAscii(char ch)
  {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  inline bool completionStartsWithInsensitive(std::string_view candidate, std::string_view prefix)
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

  inline std::string completionFrequencyDetail(std::uint32_t frequency)
  {
    return std::to_string(frequency);
  }

  template<typename MakeItem>
  void appendVocabularyCompletionItems(std::vector<CompletionItem>& items,
                                       std::span<VocabularyEntry const> vocabulary,
                                       std::string_view prefix,
                                       std::size_t limit,
                                       MakeItem makeItem)
  {
    for (auto const& entry : vocabulary)
    {
      if (items.size() >= limit)
      {
        return;
      }

      if (!completionStartsWithInsensitive(entry.value, prefix))
      {
        continue;
      }

      auto item = makeItem(entry);
      item.rank = static_cast<std::uint32_t>(items.size());
      items.push_back(std::move(item));
    }
  }
} // namespace ao::rt
