// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/completion/CompletionService.h>

#include <concepts>
#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace ao::rt
{
  /** Matches an alias only when the value has no direct/word match; aliasPrefix comes from
   * makeCompletionAliasPrefixKey. */
  bool matchesCompletionVocabularyAlias(VocabularyEntry const& entry,
                                        std::string_view prefix,
                                        std::string_view aliasPrefix);

  /**
   * Selects at most limit entries in direct, word, then alias buckets while preserving input order.
   * Alias-only values are appended once; distinct values sharing an alias remain distinct.
   * Returned pointers borrow vocabulary storage and must not outlive or cross invalidation of that vocabulary.
   */
  std::vector<VocabularyEntry const*> selectCompletionVocabularyEntries(std::span<VocabularyEntry const> vocabulary,
                                                                        std::string_view prefix,
                                                                        std::size_t limit);

  template<typename Range>
    requires std::ranges::range<Range> && (!std::ranges::borrowed_range<Range>) &&
               std::convertible_to<Range&&, std::span<VocabularyEntry const>>
  void selectCompletionVocabularyEntries(Range&&, std::string_view, std::size_t) = delete;
} // namespace ao::rt
