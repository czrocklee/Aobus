// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Internal vocabulary matching and rendering helpers for ao_app_runtime completion code.

#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/CompletionText.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
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

    auto wordMatches = std::vector<VocabularyEntry const*>{};
    wordMatches.reserve(std::min(limit - items.size(), vocabulary.size()));

    auto appendItem = [&](VocabularyEntry const& entry)
    {
      auto item = makeItem(entry);
      item.rank = static_cast<std::uint32_t>(items.size());
      items.push_back(std::move(item));
    };

    for (auto const& entry : vocabulary)
    {
      auto const optMatchOffset = findCompletionWordPrefixInsensitive(entry.value, prefix);

      if (!optMatchOffset)
      {
        continue;
      }

      if (*optMatchOffset != 0)
      {
        if (wordMatches.size() < limit - items.size())
        {
          wordMatches.push_back(&entry);
        }

        continue;
      }

      appendItem(entry);

      if (items.size() >= limit)
      {
        return;
      }
    }

    for (auto const* const entry : wordMatches)
    {
      appendItem(*entry);

      if (items.size() >= limit)
      {
        return;
      }
    }

    auto const optAliasPrefix = makeCompletionAliasPrefixKey(prefix);

    if (!optAliasPrefix)
    {
      return;
    }

    for (auto const& entry : vocabulary)
    {
      if (items.size() >= limit)
      {
        return;
      }

      if (entry.aliases.empty() ||
          std::ranges::none_of(
            entry.aliases, [&](std::string_view const alias) { return alias.starts_with(*optAliasPrefix); }) ||
          findCompletionWordPrefixInsensitive(entry.value, prefix))
      {
        continue;
      }

      appendItem(entry);
    }
  }
} // namespace ao::rt
