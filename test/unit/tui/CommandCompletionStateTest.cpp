// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/CommandCompletionState.h"

#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>

namespace ao::tui::test
{
  namespace
  {
    rt::CompletionResult completionResult()
    {
      return rt::CompletionResult{
        .replaceBegin = 5,
        .replaceEnd = 7,
        .items =
          {
            rt::CompletionItem{
              .displayText = "songs", .insertText = "songs", .detail = rt::CompletionDetail::makeResolvedText("view")},
            rt::CompletionItem{.displayText = "albums",
                               .insertText = "albums",
                               .detail = rt::CompletionDetail::makeResolvedText("view")},
          },
      };
    }

    rt::CompletionResult pageCompletionResult()
    {
      auto result = completionResult();

      while (result.items.size() < 12)
      {
        result.items.push_back(rt::CompletionItem{.displayText = "item", .insertText = "item"});
      }

      return result;
    }
  } // namespace

  TEST_CASE("CommandCompletionState - clears empty and missing completion results", "[tui][unit][completion]")
  {
    auto state = CommandCompletionState{};

    state.set(completionResult());
    REQUIRE(state.result());

    state.set(std::nullopt);
    CHECK_FALSE(state.result());
    CHECK(state.selection() == 0);

    state.set(rt::CompletionResult{});
    CHECK_FALSE(state.result());
    CHECK(state.selection() == 0);
  }

  TEST_CASE("CommandCompletionState - wraps selection movement and resets on set", "[tui][unit][completion]")
  {
    auto state = CommandCompletionState{};

    state.set(completionResult());
    CHECK(state.moveSelection(-1));
    CHECK(state.selection() == 1);
    CHECK(state.moveSelection(1));
    CHECK(state.selection() == 0);

    state.moveSelection(1);
    state.set(completionResult());
    CHECK(state.selection() == 0);
  }

  TEST_CASE("CommandCompletionState - applies selected replacement inside a draft", "[tui][unit][completion]")
  {
    auto state = CommandCompletionState{};
    auto draft = std::string{"view so"};

    state.set(completionResult());
    state.moveSelection(1);

    CHECK(state.applyTo(draft));
    CHECK(draft == "view albums");
    CHECK_FALSE(state.result());
    CHECK(state.selection() == 0);
  }

  TEST_CASE("CommandCompletionState - rejects invalid replacement ranges", "[tui][regression][completion]")
  {
    auto state = CommandCompletionState{};
    auto draft = std::string{"view so"};
    auto result = completionResult();

    SECTION("reversed range")
    {
      result.replaceBegin = 7;
      result.replaceEnd = 5;
    }

    SECTION("range past the draft")
    {
      result.replaceEnd = draft.size() + 1;
    }

    state.set(std::move(result));

    CHECK_FALSE(state.applyTo(draft));
    CHECK(draft == "view so");
    CHECK_FALSE(state.result());
    CHECK(state.selection() == 0);
  }

  TEST_CASE("CommandCompletionState - page movement stops at completion boundaries", "[tui][unit][completion]")
  {
    auto state = CommandCompletionState{};
    state.set(pageCompletionResult());

    CHECK(state.moveSelectionByPage(10));
    CHECK(state.selection() == 10);
    CHECK(state.moveSelectionByPage(10));
    CHECK(state.selection() == 11);
    CHECK(state.moveSelectionByPage(-10));
    CHECK(state.selection() == 1);
    CHECK(state.moveSelectionByPage(-10));
    CHECK(state.selection() == 0);
  }
} // namespace ao::tui::test
