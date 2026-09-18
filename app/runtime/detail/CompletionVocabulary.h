// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Internal vocabulary rendering adapter for ao_app_runtime completion code.

#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/CompletionVocabulary.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  template<typename MakeItem>
  void appendVocabularyCompletionItems(std::vector<CompletionItem>& items,
                                       std::span<VocabularyEntry const> vocabulary,
                                       std::string_view prefix,
                                       std::size_t limit,
                                       MakeItem makeItem)
  {
    if (items.size() >= limit)
    {
      return;
    }

    for (auto const* const entry : selectCompletionVocabularyEntries(vocabulary, prefix, limit - items.size()))
    {
      auto item = makeItem(*entry);
      item.rank = static_cast<std::uint32_t>(items.size());
      items.push_back(std::move(item));
    }
  }
} // namespace ao::rt
