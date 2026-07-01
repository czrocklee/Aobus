// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/CommandCompletionState.h"

#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

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
            rt::CompletionItem{.displayText = "songs", .insertText = "songs", .detail = "view"},
            rt::CompletionItem{.displayText = "albums", .insertText = "albums", .detail = "view"},
          },
      };
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

  TEST_CASE("CommandCompletionState - clamps selection movement and resets on set", "[tui][unit][completion]")
  {
    auto state = CommandCompletionState{};

    state.set(completionResult());
    CHECK(state.moveSelection(99));
    CHECK(state.selection() == 1);
    CHECK(state.moveSelection(-99));
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
} // namespace ao::tui::test
