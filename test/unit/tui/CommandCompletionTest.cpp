// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/CommandCompletion.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "tui/ShellInteractionModel.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>
#include <ao/uimodel/library/track/TrackFilter.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    std::optional<rt::CompletionResult> completeDraft(i18n::MessageCatalog const& catalog,
                                                      std::string_view draft,
                                                      CommandCompletionContext const& context,
                                                      std::size_t limit = kInputCompletionResultLimit)
    {
      return completeCommandDraft(catalog, draft, draft.size(), context, limit);
    }

    std::vector<std::string> insertTexts(rt::CompletionResult const& result)
    {
      auto values = std::vector<std::string>{};
      values.reserve(result.items.size());

      for (auto const& item : result.items)
      {
        values.push_back(item.insertText);
      }

      return values;
    }
  } // namespace

  TEST_CASE("CommandCompletion - completes command names from shell command specs", "[tui][unit][completion]")
  {
    auto const optResult = completeDraft(ao::test::englishMessageCatalog(), "ou", CommandCompletionContext{});

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 0);
    CHECK(optResult->replaceEnd == 2);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"output", "previous", "back"});
    CHECK(optResult->items[0].displayText == "output device");
    CHECK(uimodel::completionDetail(ao::test::englishMessageCatalog(), optResult->items[0].detail) == ":output");

    // Completing an already-canonical command from within its name must not
    // duplicate the text after the caret.
    auto const optInterior = completeCommandDraft(ao::test::englishMessageCatalog(), "output", 2, {});
    REQUIRE(optInterior);
    CHECK(optInterior->replaceBegin == 0);
    CHECK(optInterior->replaceEnd == 6);
    REQUIRE_FALSE(optInterior->items.empty());
    CHECK(optInterior->items.front().insertText == "output");
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "output");
    REQUIRE(shell.tryMoveInputCursor(2));
    CHECK(shell.inputField().cursor() == 2);
    shell.setCommandCompletion(optInterior);
    REQUIRE(shell.tryApplyCommandCompletion());
    CHECK(shell.inputDraft() == "output");
    CHECK(shell.inputField().cursor() == 6);
  }

  TEST_CASE("CommandCompletion - completes presentation ids after view commands", "[tui][unit][completion]")
  {
    auto const optResult =
      completeDraft(ao::test::englishMessageCatalog(),
                    "view al",
                    CommandCompletionContext{.builtinPresentations = rt::builtinTrackPresentationPresets()});

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 5);
    CHECK(optResult->replaceEnd == 7);
    CHECK(optResult->items[0].insertText == "albums");
    CHECK(uimodel::completionDetail(ao::test::englishMessageCatalog(), optResult->items[0].detail) == "Albums");

    // The current preset id is one complete token even with the caret inside it.
    auto const optInterior =
      completeCommandDraft(ao::test::englishMessageCatalog(),
                           "view artists",
                           7,
                           CommandCompletionContext{.builtinPresentations = rt::builtinTrackPresentationPresets()});
    REQUIRE(optInterior);
    CHECK(optInterior->replaceBegin == 5);
    CHECK(optInterior->replaceEnd == 12);
    REQUIRE_FALSE(optInterior->items.empty());
    CHECK(optInterior->items.front().insertText == "artists");
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "view artists");
    REQUIRE(shell.tryMoveInputCursor(7));
    CHECK(shell.inputField().cursor() == 7);
    shell.setCommandCompletion(optInterior);
    REQUIRE(shell.tryApplyCommandCompletion());
    CHECK(shell.inputDraft() == "view artists");
    CHECK(shell.inputField().cursor() == 12);
  }

  TEST_CASE("CommandCompletion - returns no filter result without a filter completion provider",
            "[tui][unit][completion]")
  {
    CHECK_FALSE(completeDraft(ao::test::englishMessageCatalog(), "filter Road Trips", CommandCompletionContext{}));
  }

  TEST_CASE("CommandCompletion - returns no result for unmatched command and presentation prefixes",
            "[tui][unit][completion]")
  {
    CHECK_FALSE(completeDraft(ao::test::englishMessageCatalog(), "zzz", CommandCompletionContext{}));
    CHECK_FALSE(completeDraft(ao::test::englishMessageCatalog(), "zzzzzz", CommandCompletionContext{}));
    CHECK_FALSE(completeDraft(ao::test::englishMessageCatalog(),
                              "view zzz",
                              CommandCompletionContext{.builtinPresentations = rt::builtinTrackPresentationPresets()}));
  }

  TEST_CASE("CommandCompletion - offers multi-word exact aliases from a prefix", "[tui][unit][completion]")
  {
    auto const optScan = completeDraft(ao::test::englishMessageCatalog(), "scan", CommandCompletionContext{});

    REQUIRE(optScan);
    CHECK(optScan->replaceBegin == 0);
    CHECK(optScan->replaceEnd == 4);
    CHECK(insertTexts(*optScan) == std::vector<std::string>{"scan", "scan cancel"});

    auto const optTrailingSpace = completeDraft(ao::test::englishMessageCatalog(), "scan ", CommandCompletionContext{});

    REQUIRE(optTrailingSpace);
    CHECK(optTrailingSpace->replaceBegin == 0);
    CHECK(optTrailingSpace->replaceEnd == 5);
    CHECK(insertTexts(*optTrailingSpace) == std::vector<std::string>{"scan", "scan cancel"});

    auto const optPartial = completeDraft(ao::test::englishMessageCatalog(), "scan c", CommandCompletionContext{});

    REQUIRE(optPartial);
    CHECK(insertTexts(*optPartial) == std::vector<std::string>{"scan cancel"});

    auto const optSelect = completeDraft(ao::test::englishMessageCatalog(), "select", CommandCompletionContext{});
    REQUIRE(optSelect);
    CHECK(insertTexts(*optSelect) ==
          std::vector<std::string>{"select toggle", "select visual", "select all", "select clear"});
  }

  TEST_CASE("CommandCompletion - finds quick tags by command prefix", "[tui][unit][completion]")
  {
    auto const optResult = completeDraft(ao::test::englishMessageCatalog(), "tag", CommandCompletionContext{});
    REQUIRE(optResult);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"tags"});
  }

  TEST_CASE("CommandCompletion - limits command candidates", "[tui][unit][completion]")
  {
    auto const optResult = completeDraft(ao::test::englishMessageCatalog(), "ou", CommandCompletionContext{}, 1);

    REQUIRE(optResult);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"output"});
  }

  TEST_CASE("CommandCompletion - delegates explicit filter arguments to the shared filter completer",
            "[tui][unit][completion][filter]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
                                                library::test::TrackSpec{.title = "Expression Track",
                                                                         .artist = "Aimer",
                                                                         .uri = "tui-expression-completion.flac",
                                                                         .duration = std::chrono::seconds{120}});
    auto changes = rt::test::makeStateOnlyLibraryChanges(libraryFixture.library());
    auto service = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = uimodel::TrackFilterCompleter{service};
    auto context = CommandCompletionContext{
      .filterCompleter = [&](std::string_view const text, std::size_t const cursor, std::size_t const limit)
        -> std::optional<rt::CompletionResult> { return completer.complete(text, cursor, limit); },
    };

    auto optResult = completeDraft(ao::test::englishMessageCatalog(), "filter $ar", context);

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 7);
    CHECK(optResult->replaceEnd == 10);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"$artist"});
    CHECK(uimodel::completionDetail(ao::test::englishMessageCatalog(), optResult->items[0].detail) == "field");

    optResult = completeDraft(ao::test::englishMessageCatalog(), "filter $artist = Ai", context);

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 17);
    CHECK(optResult->replaceEnd == 19);
    CHECK(optResult->items[0].displayText == "Aimer");
    CHECK(optResult->items[0].insertText == "\"Aimer\"");
  }

  TEST_CASE("CommandCompletion - translates an interior filter range into shell offsets",
            "[tui][unit][completion][input]")
  {
    std::size_t calls = 0;
    auto const optCompletion = completeCommandDraft(
      ao::test::englishMessageCatalog(),
      "filter $ar = Aimer",
      10,
      CommandCompletionContext{.filterCompleter = [&calls](std::string_view text, std::size_t cursor, std::size_t)
                               {
                                 ++calls;
                                 CHECK(text == "$ar = Aimer");
                                 CHECK(cursor == 3);
                                 return rt::CompletionResult{
                                   .replaceBegin = 0,
                                   .replaceEnd = 3,
                                   .items = {rt::CompletionItem{.displayText = "$artist", .insertText = "$artist"}}};
                               }});
    REQUIRE(optCompletion);
    CHECK(calls == 1);
    CHECK(optCompletion->replaceBegin == 7);
    CHECK(optCompletion->replaceEnd == 10);
    REQUIRE(optCompletion->items.size() == 1);
    CHECK(optCompletion->items.front().displayText == "$artist");
    CHECK(optCompletion->items.front().insertText == "$artist");
  }

  TEST_CASE("CommandCompletion - action search deduplicates aliases and accepts localized names",
            "[tui][unit][completion][input]")
  {
    auto const catalog = ao::test::messageCatalog("zh-Hans");
    auto const optByName = completeCommandDraft(catalog, "设置", std::string{"设置"}.size(), {});
    REQUIRE(optByName);
    REQUIRE(optByName->items.size() == 1);
    CHECK(optByName->replaceBegin == 0);
    CHECK(optByName->replaceEnd == std::string{"设置"}.size());
    CHECK(optByName->items.front().displayText == "设置");
    CHECK(optByName->items.front().insertText == "settings");
    auto const optByAlias = completeCommandDraft(catalog, "device", 6, {});
    REQUIRE(optByAlias);
    REQUIRE(optByAlias->items.size() == 1);
    CHECK(optByAlias->replaceBegin == 0);
    CHECK(optByAlias->replaceEnd == 6);
    CHECK(optByAlias->items.front().insertText == "output");
    auto const optAbbreviated = completeCommandDraft(catalog, "stgs", 4, {});
    REQUIRE(optAbbreviated);
    REQUIRE(optAbbreviated->items.size() == 1);
    CHECK(optAbbreviated->replaceBegin == 0);
    CHECK(optAbbreviated->replaceEnd == 4);
    CHECK(optAbbreviated->items.front().insertText == "settings");
  }

  TEST_CASE("CommandCompletion - custom presentation uses its id and label", "[tui][unit][completion]")
  {
    auto const custom = std::vector<rt::CustomTrackPresentationPreset>{
      {.label = "My listening view", .spec = {.id = "my-listening-view"}}};
    auto const optResult = completeDraft(
      ao::test::englishMessageCatalog(), "view my-l", CommandCompletionContext{.customPresentations = custom});
    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 5);
    CHECK(optResult->replaceEnd == 9);
    REQUIRE(optResult->items.size() == 1);
    CHECK(optResult->items.front().displayText == "my-listening-view");
    CHECK(optResult->items.front().insertText == "my-listening-view");
    CHECK(uimodel::completionDetail(ao::test::englishMessageCatalog(), optResult->items.front().detail) ==
          "My listening view");
  }

  TEST_CASE("CommandCompletion - zero limit and invalid caret refuse candidates without forwarding",
            "[tui][unit][completion]")
  {
    std::size_t calls = 0;
    auto const context = CommandCompletionContext{
      .filterCompleter = [&calls](std::string_view, std::size_t, std::size_t) -> std::optional<rt::CompletionResult>
      {
        ++calls;
        return std::nullopt;
      }};
    auto const& catalog = ao::test::englishMessageCatalog();
    CHECK_FALSE(completeCommandDraft(catalog, "filter A", 9, context));
    CHECK_FALSE(completeCommandDraft(catalog, "ou", 3, context));
    CHECK(calls == 0);
    CHECK_FALSE(completeCommandDraft(catalog, "ou", 2, context, 0));
    CHECK_FALSE(
      completeCommandDraft(catalog,
                           "view al",
                           7,
                           CommandCompletionContext{.builtinPresentations = rt::builtinTrackPresentationPresets()},
                           0));
  }

  TEST_CASE("CommandCompletion - filter callback receives the exact argument cursor and limit",
            "[tui][unit][completion][filter]")
  {
    std::size_t calls = 0;
    auto const optResult = completeCommandDraft(
      ao::test::englishMessageCatalog(),
      "filter two  words",
      11,
      CommandCompletionContext{.filterCompleter = [&calls](std::string_view text, std::size_t cursor, std::size_t limit)
                                 -> std::optional<rt::CompletionResult>
                               {
                                 ++calls;
                                 CHECK(text == "two  words");
                                 CHECK(cursor == 4);
                                 CHECK(limit == 3);
                                 return std::nullopt;
                               }},
      3);
    CHECK_FALSE(optResult);
    CHECK(calls == 1);
  }
} // namespace ao::tui::test
