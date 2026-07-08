// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/CommandCompletion.h"

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "tui/Model.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>
#include <ao/rt/library/LibraryChanges.h>

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
    auto const optResult = completeCommandDraft("ou", CommandCompletionContext{});

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 0);
    CHECK(optResult->replaceEnd == 2);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"output", "outputs"});
    CHECK(optResult->items[0].displayText == "/output");
    CHECK(optResult->items[0].detail == "output device");
  }

  TEST_CASE("CommandCompletion - completes presentation ids after view commands", "[tui][unit][completion]")
  {
    auto const optResult = completeCommandDraft(
      "view al", CommandCompletionContext{.builtinPresentations = rt::builtinTrackPresentationPresets()});

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 5);
    CHECK(optResult->replaceEnd == 7);
    CHECK(optResult->items[0].insertText == "albums");
    CHECK(optResult->items[0].detail == "Albums");
  }

  TEST_CASE("CommandCompletion - completes artists and lists for quick filters", "[tui][unit][completion]")
  {
    auto const lists =
      std::vector<LibraryNavItem>{{.label = "[#] Road Trips", .detail = {}, .completionText = "Road Trips"}};
    auto const artists = std::vector<rt::VocabularyEntry>{{.value = "Aimer", .frequency = 7}};

    auto optResult = completeCommandDraft("Ai", CommandCompletionContext{.lists = lists, .artists = artists});

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 0);
    CHECK(optResult->replaceEnd == 2);
    CHECK(optResult->items[0].insertText == "Aimer");
    CHECK(optResult->items[0].detail == "artist");

    optResult = completeCommandDraft("filter Ro", CommandCompletionContext{.lists = lists, .artists = artists});

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 7);
    CHECK(optResult->replaceEnd == 9);
    CHECK(optResult->items[0].insertText == "Road Trips");
    CHECK(optResult->items[0].detail == "list");
  }

  TEST_CASE("CommandCompletion - bare quick filters match the whole draft for multi-word names",
            "[tui][unit][completion]")
  {
    auto const lists =
      std::vector<LibraryNavItem>{{.label = "[#] Road Trips", .detail = {}, .completionText = "Road Trips"}};
    auto const artists = std::vector<rt::VocabularyEntry>{
      {.value = "Road The Beatles", .frequency = 1},
      {.value = "Road Trip Artist", .frequency = 1},
    };

    auto const optResult = completeCommandDraft("Road T", CommandCompletionContext{.lists = lists, .artists = artists});

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 0);
    CHECK(optResult->replaceEnd == 6);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"Road The Beatles", "Road Trip Artist", "Road Trips"});
  }

  TEST_CASE("CommandCompletion - returns no result for unmatched command and presentation prefixes",
            "[tui][unit][completion]")
  {
    CHECK_FALSE(completeCommandDraft("zzz", CommandCompletionContext{}));
    CHECK_FALSE(completeCommandDraft(
      "view zzz", CommandCompletionContext{.builtinPresentations = rt::builtinTrackPresentationPresets()}));
  }

  TEST_CASE("CommandCompletion - limits scanning after enough candidates", "[tui][unit][completion]")
  {
    auto const artists = std::vector<rt::VocabularyEntry>{
      {.value = "Aimer", .frequency = 7},
      {.value = "Akiko Yano", .frequency = 3},
    };

    auto const optResult = completeCommandDraft("Ai", CommandCompletionContext{.artists = artists}, 1);

    REQUIRE(optResult);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"Aimer"});
  }

  TEST_CASE("CommandCompletion - non-leading expression punctuation stays in quick-filter completion",
            "[tui][unit][completion]")
  {
    bool expressionCalled = false;
    auto const artists = std::vector<rt::VocabularyEntry>{{.value = "P!nk", .frequency = 4}};
    auto const context = CommandCompletionContext{
      .artists = artists,
      .expressionCompleter = [&](std::string_view, std::size_t, std::size_t) -> std::optional<rt::CompletionResult>
      {
        expressionCalled = true;
        return rt::CompletionResult{
          .items = {rt::CompletionItem{.displayText = "$artist", .insertText = "$artist", .detail = "field"}},
        };
      },
    };

    auto const optResult = completeCommandDraft("P!", context);

    REQUIRE(optResult);
    CHECK_FALSE(expressionCalled);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"P!nk"});
  }

  TEST_CASE("CommandCompletion - delegates expression completion to runtime query completer",
            "[tui][unit][completion][query]")
  {
    auto testLib = rt::test::TestMusicLibrary{};
    library::test::addTrack(testLib.library(),
                            library::test::TrackSpec{.title = "Expression Track",
                                                     .artist = "Aimer",
                                                     .uri = "/tmp/tui-expression-completion.flac",
                                                     .duration = std::chrono::seconds{120}});
    auto changes = rt::LibraryChanges{};
    auto service = rt::CompletionService{testLib.library(), changes};
    auto completer = rt::QueryExpressionCompleter{service};
    auto context = CommandCompletionContext{
      .expressionCompleter = [&](std::string_view const text, std::size_t const cursor, std::size_t const limit)
        -> std::optional<rt::CompletionResult> { return completer.complete(text, cursor, limit); },
    };

    auto optResult = completeCommandDraft("filter $ar", context);

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 7);
    CHECK(optResult->replaceEnd == 10);
    CHECK(insertTexts(*optResult) == std::vector<std::string>{"$artist"});
    CHECK(optResult->items[0].detail == "field");

    optResult = completeCommandDraft("filter $artist = Ai", context);

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 17);
    CHECK(optResult->replaceEnd == 19);
    CHECK(optResult->items[0].displayText == "Aimer");
    CHECK(optResult->items[0].insertText == "\"Aimer\"");
  }
} // namespace ao::tui::test
