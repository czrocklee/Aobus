// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/CommandCompletion.h"

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/tui/TuiTextCatalogTestSupport.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/uimodel/library/track/TrackFilterCompleter.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

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
    std::optional<rt::CompletionResult> completeCommandDraft(uimodel::PresentationTextCatalog const& textCatalog,
                                                             std::string_view const draft,
                                                             CommandCompletionContext const context,
                                                             std::size_t const limit = 8)
    {
      return ao::tui::completeCommandDraft(textCatalog, englishTuiTextCatalog(), draft, context, limit);
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
    auto const optResult =
      completeCommandDraft(ao::test::englishPresentationTextCatalog(), "ou", CommandCompletionContext{});

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 0);
    CHECK(optResult->replaceEnd == 2);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"output", "outputs"});
    CHECK(optResult->items[0].displayText == "/output");
    CHECK(ao::test::englishPresentationTextCatalog().completionDetail(optResult->items[0].detail) == "output device");
  }

  TEST_CASE("CommandCompletion - completes presentation ids after view commands", "[tui][unit][completion]")
  {
    auto const optResult =
      completeCommandDraft(ao::test::englishPresentationTextCatalog(),
                           "view al",
                           CommandCompletionContext{.builtinPresentations = rt::builtinTrackPresentationPresets()});

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 5);
    CHECK(optResult->replaceEnd == 7);
    CHECK(optResult->items[0].insertText == "albums");
    CHECK(ao::test::englishPresentationTextCatalog().completionDetail(optResult->items[0].detail) == "Albums");
  }

  TEST_CASE("CommandCompletion - returns no filter result without a filter completion provider",
            "[tui][unit][completion]")
  {
    CHECK_FALSE(completeCommandDraft(ao::test::englishPresentationTextCatalog(), "Aimer", CommandCompletionContext{}));
    CHECK_FALSE(completeCommandDraft(
      ao::test::englishPresentationTextCatalog(), "filter Road Trips", CommandCompletionContext{}));
  }

  TEST_CASE("CommandCompletion - returns no result for unmatched command and presentation prefixes",
            "[tui][unit][completion]")
  {
    CHECK_FALSE(completeCommandDraft(ao::test::englishPresentationTextCatalog(), "zzz", CommandCompletionContext{}));
    CHECK_FALSE(
      completeCommandDraft(ao::test::englishPresentationTextCatalog(),
                           "view zzz",
                           CommandCompletionContext{.builtinPresentations = rt::builtinTrackPresentationPresets()}));
  }

  TEST_CASE("CommandCompletion - limits command candidates", "[tui][unit][completion]")
  {
    auto const optResult =
      completeCommandDraft(ao::test::englishPresentationTextCatalog(), "ou", CommandCompletionContext{}, 1);

    REQUIRE(optResult);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"output"});
  }

  TEST_CASE("CommandCompletion - delegates Quick and expression completion to the shared filter completer",
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

    auto optResult = completeCommandDraft(ao::test::englishPresentationTextCatalog(), "Aim", context);

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 0);
    CHECK(optResult->replaceEnd == 3);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"\"Aimer\""});

    optResult = completeCommandDraft(ao::test::englishPresentationTextCatalog(), "filter $ar", context);

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 7);
    CHECK(optResult->replaceEnd == 10);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"$artist"});
    CHECK(ao::test::englishPresentationTextCatalog().completionDetail(optResult->items[0].detail) == "field");

    optResult = completeCommandDraft(ao::test::englishPresentationTextCatalog(), "filter $artist = Ai", context);

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 17);
    CHECK(optResult->replaceEnd == 19);
    CHECK(optResult->items[0].displayText == "Aimer");
    CHECK(optResult->items[0].insertText == "\"Aimer\"");
  }
} // namespace ao::tui::test
