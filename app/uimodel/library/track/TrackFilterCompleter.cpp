// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackFilterCompleter.h>

#include "TrackFilterPolicy.h"
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/CompletionText.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    bool rankedBefore(rt::VocabularyEntry const& lhs, rt::VocabularyEntry const& rhs)
    {
      return lhs.frequency > rhs.frequency || (lhs.frequency == rhs.frequency && lhs.value < rhs.value);
    }

    void insertRanked(std::vector<rt::VocabularyEntry const*>& matches,
                      rt::VocabularyEntry const& entry,
                      std::size_t const limit)
    {
      auto const position = std::ranges::lower_bound(matches,
                                                     &entry,
                                                     [](rt::VocabularyEntry const* lhs, rt::VocabularyEntry const* rhs)
                                                     { return rankedBefore(*lhs, *rhs); });

      if (matches.size() < limit)
      {
        matches.insert(position, &entry);
      }
      else if (position != matches.end())
      {
        matches.insert(position, &entry);
        matches.pop_back();
      }
    }

    std::vector<rt::VocabularyEntry const*> bestMatches(std::span<rt::VocabularyEntry const> vocabulary,
                                                        std::string_view prefix,
                                                        std::size_t limit)
    {
      auto directMatches = std::vector<rt::VocabularyEntry const*>{};
      auto wordMatches = std::vector<rt::VocabularyEntry const*>{};
      directMatches.reserve(limit + 1);
      wordMatches.reserve(limit + 1);

      for (auto const& entry : vocabulary)
      {
        auto const optMatchOffset = rt::findCompletionWordPrefixInsensitive(entry.value, prefix);

        if (!optMatchOffset)
        {
          continue;
        }

        insertRanked(*optMatchOffset == 0 ? directMatches : wordMatches, entry, limit);
      }

      auto matches = std::move(directMatches);

      if (matches.size() < limit)
      {
        auto const wordCount = std::min(wordMatches.size(), limit - matches.size());

        for (std::size_t index = 0; index < wordCount; ++index)
        {
          matches.push_back(wordMatches[index]);
        }
      }

      if (matches.size() >= limit)
      {
        return matches;
      }

      auto const optAliasPrefix = rt::makeCompletionAliasPrefixKey(prefix);

      if (!optAliasPrefix)
      {
        return matches;
      }

      auto aliasMatches = std::vector<rt::VocabularyEntry const*>{};
      auto const aliasLimit = limit - matches.size();
      aliasMatches.reserve(aliasLimit + 1);

      for (auto const& entry : vocabulary)
      {
        if (entry.aliases.empty() ||
            std::ranges::none_of(
              entry.aliases, [&](std::string_view const alias) { return alias.starts_with(*optAliasPrefix); }) ||
            rt::findCompletionWordPrefixInsensitive(entry.value, prefix))
        {
          continue;
        }

        insertRanked(aliasMatches, entry, aliasLimit);
      }

      matches.insert(matches.end(), aliasMatches.begin(), aliasMatches.end());
      return matches;
    }
  } // namespace

  TrackFilterCompleter::TrackFilterCompleter(rt::CompletionService& vocabulary)
    : _vocabulary{vocabulary}, _expressionCompleter{vocabulary}
  {
  }

  std::optional<rt::CompletionResult> TrackFilterCompleter::complete(std::string_view text,
                                                                     std::size_t cursor,
                                                                     std::size_t limit)
  {
    if (cursor > text.size() || limit == 0)
    {
      return std::nullopt;
    }

    if (detail::isExplicitTrackFilterExpression(text))
    {
      return _expressionCompleter.complete(text, cursor, limit);
    }

    auto const optToken = detail::analyzeQuickFilterCompletion(text, cursor);

    if (!optToken)
    {
      return std::nullopt;
    }

    auto const resultLimit = std::min(limit, rt::kCompletionResultLimit);
    auto const vocabulary = _vocabulary.aggregateValues(rt::TrackValueVocabularySpec{
      .fields = std::span{detail::kQuickFilterFields},
      .includeTags = true,
    });
    auto const matches = bestMatches(vocabulary, optToken->prefix, resultLimit);

    if (matches.empty())
    {
      return std::nullopt;
    }

    auto items = std::vector<rt::CompletionItem>{};
    items.reserve(matches.size());

    for (auto const* const entry : matches)
    {
      items.push_back(rt::CompletionItem{
        .displayText = entry->value,
        .insertText = query::serialize(query::ConstantExpression{entry->value}),
        .detail = rt::CompletionDetail::makeUsageFrequency(entry->frequency),
        .rank = static_cast<std::uint32_t>(items.size()),
      });
    }

    return rt::CompletionResult{
      .replaceBegin = optToken->replaceBegin,
      .replaceEnd = optToken->replaceEnd,
      .items = std::move(items),
    };
  }
} // namespace ao::uimodel
