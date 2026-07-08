// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Internal vocabulary matching and rendering helpers for ao_app_runtime completion code.

#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/CompletionText.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
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

      if (!startsWithCompletionPrefixInsensitive(entry.value, prefix))
      {
        continue;
      }

      auto item = makeItem(entry);
      item.rank = static_cast<std::uint32_t>(items.size());
      items.push_back(std::move(item));
    }
  }
} // namespace ao::rt
