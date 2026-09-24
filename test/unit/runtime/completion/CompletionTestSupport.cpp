// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/completion/CompletionTestSupport.h"

#include <ao/rt/completion/CompletionItem.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  std::vector<std::string> insertTexts(std::vector<CompletionItem> const& items)
  {
    auto result = std::vector<std::string>{};

    for (auto const& item : items)
    {
      result.push_back(item.insertText);
    }

    return result;
  }

  void checkCompletionItem(CompletionItem const& item,
                           std::string_view displayText,
                           std::string_view insertText,
                           CompletionDetailKind detailKind,
                           std::uint32_t frequency,
                           std::uint32_t rank)
  {
    CHECK(item.displayText == displayText);
    CHECK(item.insertText == insertText);
    CHECK(item.detail.kind == detailKind);
    CHECK(item.detail.frequency == frequency);
    CHECK(item.rank == rank);
  }
} // namespace ao::rt::test
