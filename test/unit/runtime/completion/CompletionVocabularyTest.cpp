// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/completion/CompletionVocabulary.h>

#include "runtime/detail/CompletionVocabulary.h"
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionService.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    template<typename Range>
    concept SelectableVocabularyRange = requires(Range&& range) {
      {
        selectCompletionVocabularyEntries(std::forward<Range>(range), "", 1)
      } -> std::same_as<std::vector<VocabularyEntry const*>>;
    };

    static_assert(SelectableVocabularyRange<std::vector<VocabularyEntry>&>);
    static_assert(SelectableVocabularyRange<std::array<VocabularyEntry, 2>&>);
    static_assert(!SelectableVocabularyRange<std::vector<VocabularyEntry>>);
    static_assert(!SelectableVocabularyRange<std::array<VocabularyEntry, 2>>);
    static_assert(SelectableVocabularyRange<std::span<VocabularyEntry>>);
    static_assert(SelectableVocabularyRange<std::span<VocabularyEntry const>>);
    static_assert(SelectableVocabularyRange<std::ranges::subrange<VocabularyEntry*>>);

    std::vector<std::string> selectedValues(std::span<VocabularyEntry const> vocabulary,
                                            std::string_view prefix,
                                            std::size_t limit)
    {
      auto values = std::vector<std::string>{};

      for (auto const* const entry : selectCompletionVocabularyEntries(vocabulary, prefix, limit))
      {
        values.push_back(entry->value);
      }

      return values;
    }
  } // namespace

  TEST_CASE("selectCompletionVocabularyEntries preserves bucket and input order within a bound",
            "[runtime][unit][completion]")
  {
    auto const alias = std::array<std::string, 1>{"zhoujielun"};
    auto const vocabulary = std::array{
      VocabularyEntry{.value = "Trevor Zhou", .aliases = {}},
      VocabularyEntry{.value = "周杰倫", .aliases = alias},
      VocabularyEntry{.value = "Zhou Direct", .aliases = alias},
      VocabularyEntry{.value = "Another Zhou", .aliases = {}},
    };

    CHECK(selectedValues(vocabulary, "zhou", 4) ==
          std::vector<std::string>{"Zhou Direct", "Trevor Zhou", "Another Zhou", "周杰倫"});
    CHECK(selectedValues(vocabulary, "zhou", 2) == std::vector<std::string>{"Zhou Direct", "Trevor Zhou"});
    CHECK(selectedValues(vocabulary, "zhou", 0).empty());
  }

  TEST_CASE("selectCompletionVocabularyEntries treats an empty prefix as direct matching",
            "[runtime][unit][completion]")
  {
    auto const vocabulary = std::array{
      VocabularyEntry{.value = "First"},
      VocabularyEntry{.value = "Second"},
      VocabularyEntry{.value = "Third"},
    };

    CHECK(selectedValues(vocabulary, "", 2) == std::vector<std::string>{"First", "Second"});
  }

  TEST_CASE("selectCompletionVocabularyEntries keeps UTF-8 word boundaries and alias collisions distinct",
            "[runtime][unit][completion]")
  {
    auto const sharedAlias = std::array<std::string, 1>{"wangfei"};
    auto const directAlias = std::array<std::string, 1>{"pinnock"};
    auto const vocabulary = std::array{
      VocabularyEntry{.value = "éPinnock", .aliases = {}},
      VocabularyEntry{.value = "é-Pinnock", .aliases = directAlias},
      VocabularyEntry{.value = "王菲", .aliases = sharedAlias},
      VocabularyEntry{.value = "王妃", .aliases = sharedAlias},
    };

    CHECK(selectedValues(vocabulary, "pinnock", 4) == std::vector<std::string>{"é-Pinnock"});
    CHECK(selectedValues(vocabulary, "w-ang_fei", 4) == std::vector<std::string>{"王菲", "王妃"});
    CHECK(selectedValues(vocabulary, "wa", 4).empty());
  }

  TEST_CASE("selectCompletionVocabularyEntries deduplicates alias values without losing later candidates",
            "[runtime][unit][completion]")
  {
    auto const aliases = std::array<std::string, 1>{"wangfei"};
    auto const vocabulary = std::array{
      VocabularyEntry{.value = "王菲", .aliases = aliases},
      VocabularyEntry{.value = "王菲", .aliases = aliases},
      VocabularyEntry{.value = "王妃", .aliases = aliases},
    };

    CHECK(selectedValues(vocabulary, "wang", 2) == std::vector<std::string>{"王菲", "王妃"});

    auto const mixedVocabulary = std::array{
      vocabulary[0],
      VocabularyEntry{.value = "Later Wang", .aliases = aliases},
      vocabulary[1],
      VocabularyEntry{.value = "Wang Direct", .aliases = aliases},
      vocabulary[2],
    };

    CHECK(selectedValues(mixedVocabulary, "wang", 4) ==
          std::vector<std::string>{"Wang Direct", "Later Wang", "王菲", "王妃"});
    CHECK(selectedValues(mixedVocabulary, "wang", 3) == std::vector<std::string>{"Wang Direct", "Later Wang", "王菲"});
  }

  TEST_CASE("appendVocabularyCompletionItems respects the existing item count and absolute ranks",
            "[runtime][unit][completion]")
  {
    auto const vocabulary = std::array{
      VocabularyEntry{.value = "First"},
      VocabularyEntry{.value = "Second"},
    };
    auto items = std::vector<CompletionItem>(2);
    std::size_t renderedCount = 0;
    auto makeItem = [&](VocabularyEntry const& entry)
    {
      ++renderedCount;
      return CompletionItem{.displayText = entry.value, .insertText = entry.value};
    };

    appendVocabularyCompletionItems(items, vocabulary, "", 3, makeItem);

    REQUIRE(items.size() == 3);
    CHECK(items.back().displayText == "First");
    CHECK(items.back().rank == 2);
    CHECK(renderedCount == 1);

    appendVocabularyCompletionItems(items, vocabulary, "", 3, makeItem);
    appendVocabularyCompletionItems(items, vocabulary, "", 0, makeItem);
    CHECK(items.size() == 3);
    CHECK(renderedCount == 1);
  }
} // namespace ao::rt::test
