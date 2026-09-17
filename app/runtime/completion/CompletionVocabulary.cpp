// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/completion/CompletionVocabulary.h>

#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/CompletionText.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace ao::rt
{
  bool matchesCompletionVocabularyAlias(VocabularyEntry const& entry,
                                        std::string_view const prefix,
                                        std::string_view const aliasPrefix)
  {
    return std::ranges::any_of(
             entry.aliases, [&](std::string_view const alias) { return alias.starts_with(aliasPrefix); }) &&
           !findCompletionWordPrefixInsensitive(entry.value, prefix);
  }

  std::vector<VocabularyEntry const*> selectCompletionVocabularyEntries(
    std::span<VocabularyEntry const> const vocabulary,
    std::string_view const prefix,
    std::size_t const limit)
  {
    auto matches = std::vector<VocabularyEntry const*>{};

    if (limit == 0)
    {
      return matches;
    }

    matches.reserve(std::min(limit, vocabulary.size()));
    auto wordMatches = std::vector<VocabularyEntry const*>{};
    wordMatches.reserve(std::min(limit, vocabulary.size()));

    for (auto const& entry : vocabulary)
    {
      auto const optMatchOffset = findCompletionWordPrefixInsensitive(entry.value, prefix);

      if (!optMatchOffset)
      {
        continue;
      }

      if (*optMatchOffset != 0)
      {
        if (wordMatches.size() < limit - matches.size())
        {
          wordMatches.push_back(&entry);
        }

        continue;
      }

      matches.push_back(&entry);

      if (matches.size() >= limit)
      {
        return matches;
      }
    }

    for (auto const* const entry : wordMatches)
    {
      matches.push_back(entry);

      if (matches.size() >= limit)
      {
        return matches;
      }
    }

    // Alias work is a fallback, not part of the direct/word hot path.
    auto const optAliasKey = makeCompletionAliasPrefixKey(prefix);

    if (!optAliasKey)
    {
      return matches;
    }

    auto const aliasStart = matches.size();

    for (auto const& entry : vocabulary)
    {
      if (!matchesCompletionVocabularyAlias(entry, prefix, *optAliasKey) ||
          std::ranges::any_of(
            std::span{matches}.subspan(aliasStart), [&](auto const* match) { return match->value == entry.value; }))
      {
        continue;
      }

      matches.push_back(&entry);

      if (matches.size() >= limit)
      {
        break;
      }
    }

    return matches;
  }
} // namespace ao::rt
