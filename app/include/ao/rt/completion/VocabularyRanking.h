// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/ordering/TextOrderingPolicy.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  /**
   * Sorts by descending frequency, then by locale key (if supplied) and raw spelling.
   * Derives one key per entry. The caller handles each derivation result before sorting;
   * if it continues after failure, the entry is retained with an empty key.
   * With no policy this still sorts by frequency and raw spelling; consumers that want
   * to retain an existing order should skip the call.
   */
  template<typename Entry, typename ValueProjection, typename FrequencyProjection, typename HandleKeyResult>
  void sortVocabularyByFrequency(std::vector<Entry>& entries,
                                 TextOrderingPolicy const* textOrderingPolicy,
                                 ValueProjection valueOf,
                                 FrequencyProjection frequencyOf,
                                 HandleKeyResult handleKeyResult)
  {
    auto const precedes = [&](Entry const& lhs, std::string_view lhsKey, Entry const& rhs, std::string_view rhsKey)
    {
      if (auto const left = std::invoke(frequencyOf, lhs), right = std::invoke(frequencyOf, rhs); left != right)
      {
        return left > right;
      }

      if (auto const keyOrder = lhsKey.compare(rhsKey); keyOrder != 0)
      {
        return keyOrder < 0;
      }

      return std::string_view{std::invoke(valueOf, lhs)} < std::string_view{std::invoke(valueOf, rhs)};
    };

    if (textOrderingPolicy == nullptr)
    {
      std::ranges::sort(entries, [&](Entry const& lhs, Entry const& rhs) { return precedes(lhs, {}, rhs, {}); });
      return;
    }

    struct KeyedEntry final
    {
      Entry entry;
      std::string sortKey;
    };

    auto keyedEntries = std::vector<KeyedEntry>{};
    keyedEntries.reserve(entries.size());

    for (auto& entry : entries)
    {
      auto sortKey = std::string{};
      auto const keyRes = textOrderingPolicy->makeSortKeyInto(sortKey, std::invoke(valueOf, entry));
      std::invoke(handleKeyResult, keyRes);

      if (!keyRes)
      {
        sortKey.clear();
      }

      keyedEntries.push_back(KeyedEntry{.entry = std::move(entry), .sortKey = std::move(sortKey)});
    }

    std::ranges::sort(keyedEntries,
                      [&](KeyedEntry const& lhs, KeyedEntry const& rhs)
                      { return precedes(lhs.entry, lhs.sortKey, rhs.entry, rhs.sortKey); });

    entries.clear();

    for (auto& keyedEntry : keyedEntries)
    {
      entries.push_back(std::move(keyedEntry.entry));
    }
  }
} // namespace ao::rt
