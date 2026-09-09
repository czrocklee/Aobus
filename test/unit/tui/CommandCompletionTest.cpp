// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/CommandCompletion.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
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
  }

  TEST_CASE("CommandCompletion - returns no filter result without a filter completion provider",
            "[tui][unit][completion]")
  {
    CHECK_FALSE(completeDraft(ao::test::englishMessageCatalog(), "zzzzzz", CommandCompletionContext{}));
    CHECK_FALSE(completeDraft(ao::test::englishMessageCatalog(), "filter Road Trips", CommandCompletionContext{}));
  }

  TEST_CASE("CommandCompletion - returns no result for unmatched command and presentation prefixes",
            "[tui][unit][completion]")
  {
    CHECK_FALSE(completeDraft(ao::test::englishMessageCatalog(), "zzz", CommandCompletionContext{}));
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
} // namespace ao::tui::test
