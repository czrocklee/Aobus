// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "tui/CommandCompletion.h"
#include "tui/ShellInteractionModel.h"
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("ShellInput - edits at grapheme boundaries and distinguishes movement from typing", "[tui][unit][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "a👨‍👩‍👧‍👦b");
    CHECK_FALSE(shell.tryEditInput(ftxui::Event::ArrowLeft));
    CHECK(shell.tryEditInput(ftxui::Event::Character("X")));
    CHECK(shell.inputDraft() == "a👨‍👩‍👧‍👦Xb");
    CHECK(shell.tryEditInput(ftxui::Event::Backspace));
    CHECK(shell.tryEditInput(ftxui::Event::Backspace));
    CHECK(shell.inputDraft() == "ab");
    CHECK(shell.inputField().cursor() == 1);
    CHECK(shell.tryEditInput(ftxui::Event::Delete));
    CHECK(shell.inputDraft() == "a");
  }

  TEST_CASE("ShellInput - word editing preserves Unicode text around the cursor", "[tui][unit][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "filter 中文 音乐");
    CHECK_FALSE(shell.tryEditInput(ftxui::Event::Special("\x1b"
                                                         "b")));
    CHECK(shell.tryEditInput(ftxui::Event::CtrlW));
    CHECK(shell.inputDraft() == "filter 音乐");
    CHECK(shell.inputField().cursor() == 7);
    CHECK_FALSE(shell.tryEditInput(ftxui::Event::CtrlA));
    CHECK(shell.tryEditInput(ftxui::Event::Character(":")));
    CHECK(shell.inputDraft() == ":filter 音乐");
    CHECK_FALSE(shell.tryEditInput(ftxui::Event::CtrlE));
    CHECK(shell.inputField().cursor() == shell.inputDraft().size());
  }

  TEST_CASE("ShellInput - deleting an empty line does not mark an untouched filter as edited",
            "[tui][regression][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::QuickFilter);
    CHECK_FALSE(shell.tryEditInput(ftxui::Event::CtrlU));
    CHECK_FALSE(shell.tryEditInput(ftxui::Event::CtrlK));
    CHECK_FALSE(shell.isInputTouched());
    REQUIRE(shell.tryEditInput(ftxui::Event::Character("one two")));
    shell.tryEditInput(ftxui::Event::ArrowLeft);
    REQUIRE(shell.tryEditInput(ftxui::Event::CtrlU));
    CHECK(shell.inputDraft() == "o");
    REQUIRE(shell.tryEditInput(ftxui::Event::CtrlK));
    CHECK(shell.inputDraft().empty());
  }

  TEST_CASE("ShellInput - history is mode-local and restores the unfinished draft", "[tui][unit][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "detail");
    shell.rememberInput();
    shell.beginInput(ShellInputMode::Command, "settings");
    shell.rememberInput();
    shell.beginInput(ShellInputMode::QuickFilter, "artist");
    shell.rememberInput();
    shell.beginInput(ShellInputMode::Command, "unsent");
    REQUIRE(shell.tryMoveInputHistory(-1));
    CHECK(shell.inputDraft() == "settings");
    REQUIRE(shell.tryMoveInputHistory(-1));
    CHECK(shell.inputDraft() == "detail");
    CHECK_FALSE(shell.tryMoveInputHistory(-1));
    REQUIRE(shell.tryMoveInputHistory(1));
    REQUIRE(shell.tryMoveInputHistory(1));
    CHECK(shell.inputDraft() == "unsent");
    shell.beginInput(ShellInputMode::QuickFilter);
    REQUIRE(shell.tryMoveInputHistory(-1));
    CHECK(shell.inputDraft() == "artist");
  }

  TEST_CASE("ShellInput - editing a history entry creates a new scratch draft", "[tui][unit][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "filter A");
    shell.rememberInput();
    shell.beginInput(ShellInputMode::Command);
    REQUIRE(shell.tryMoveInputHistory(-1));
    REQUIRE(shell.tryEditInput(ftxui::Event::Character("B")));
    CHECK_FALSE(shell.tryMoveInputHistory(1));
    REQUIRE(shell.tryMoveInputHistory(-1));
    CHECK(shell.inputDraft() == "filter A");
    REQUIRE(shell.tryMoveInputHistory(1));
    CHECK(shell.inputDraft() == "filter AB");
  }

  TEST_CASE("ShellInput - cursor-aware completion replaces only its interior range", "[tui][unit][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "filter $ar = Aimer");
    auto const optCompletion = completeCommandDraft(
      ao::test::englishMessageCatalog(),
      shell.inputDraft(),
      10,
      CommandCompletionContext{.filterCompleter = [](std::string_view text, std::size_t cursor, std::size_t)
                               {
                                 CHECK(text == "$ar = Aimer");
                                 CHECK(cursor == 3);
                                 return rt::CompletionResult{
                                   .replaceBegin = 0,
                                   .replaceEnd = 3,
                                   .items = {rt::CompletionItem{.displayText = "$artist", .insertText = "$artist"}}};
                               }});
    REQUIRE(optCompletion);
    shell.setCommandCompletion(optCompletion);
    REQUIRE(shell.tryApplyCommandCompletion());
    CHECK(shell.inputDraft() == "filter $artist = Aimer");
    CHECK(shell.inputField().cursor() == 14);
  }

  TEST_CASE("ShellInput - action search deduplicates aliases and accepts localized names", "[tui][unit][input]")
  {
    auto const catalog = ao::test::messageCatalog("zh-Hans");
    auto const optByName = completeCommandDraft(catalog, "设置", std::string{"设置"}.size(), {});
    REQUIRE(optByName);
    REQUIRE(optByName->items.size() == 1);
    CHECK(optByName->items.front().insertText == "settings");
    auto const optByAlias = completeCommandDraft(catalog, "device", 6, {});
    REQUIRE(optByAlias);
    REQUIRE(optByAlias->items.size() == 1);
    CHECK(optByAlias->items.front().insertText == "output");
    auto const optAbbreviated = completeCommandDraft(catalog, "stgs", 4, {});
    REQUIRE(optAbbreviated);
    CHECK(optAbbreviated->items.front().insertText == "settings");
  }
} // namespace ao::tui::test
