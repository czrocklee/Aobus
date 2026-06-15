// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/CompletionVocabulary.h"
#include <ao/rt/CompletionItem.h>
#include <ao/rt/CompletionResult.h>
#include <ao/rt/CompletionService.h>
#include <ao/rt/MetadataValueCompleter.h>
#include <ao/rt/TrackField.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    std::vector<CompletionItem> completeMetadataValuePrefix(CompletionService& vocabulary,
                                                            TrackField field,
                                                            std::string_view prefix,
                                                            std::size_t limit)
    {
      auto items = std::vector<CompletionItem>{};

      if (limit == 0 || !trackFieldSupportsValueCompletion(field))
      {
        return items;
      }

      items.reserve(std::min(limit, kCompletionResultLimit));

      appendVocabularyCompletionItems(items,
                                      vocabulary.valuesFor(field),
                                      prefix,
                                      limit,
                                      [](VocabularyEntry const& entry)
                                      {
                                        return CompletionItem{
                                          .displayText = entry.value,
                                          .insertText = entry.value,
                                          .detail = completionFrequencyDetail(entry.frequency),
                                        };
                                      });

      return items;
    }

    std::optional<CompletionResult> completeMetadataValueEntry(CompletionService& vocabulary,
                                                               TrackField field,
                                                               std::string_view text,
                                                               std::size_t cursor,
                                                               std::size_t limit)
    {
      auto const clampedCursor = std::min(cursor, text.size());
      auto items = completeMetadataValuePrefix(vocabulary, field, text.substr(0, clampedCursor), limit);

      if (items.empty())
      {
        return std::nullopt;
      }

      return CompletionResult{
        .replaceBegin = 0,
        .replaceEnd = text.size(),
        .items = std::move(items),
      };
    }
  } // namespace

  MetadataValueCompleter::MetadataValueCompleter(CompletionService& vocabulary, TrackField field)
    : _vocabulary{vocabulary}, _field{field}
  {
  }

  std::vector<CompletionItem> MetadataValueCompleter::complete(std::string_view prefix, std::size_t limit)
  {
    return completeMetadataValuePrefix(_vocabulary, _field, prefix, limit);
  }

  CompletionProvider MetadataValueCompleter::asProvider() const
  {
    return [vocabulary = &_vocabulary, field = _field](
             std::string_view text, std::size_t cursor) -> std::optional<CompletionResult>
    { return completeMetadataValueEntry(*vocabulary, field, text, cursor, kCompletionResultLimit); };
  }
} // namespace ao::rt
