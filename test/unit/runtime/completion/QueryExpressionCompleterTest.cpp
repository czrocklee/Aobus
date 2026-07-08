// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>
#include <ao/rt/library/LibraryChanges.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void addCompletionTrack(TestMusicLibrary& testLib,
                            std::span<std::string const> tags,
                            std::span<std::pair<std::string, std::string> const> custom)
    {
      library::test::addTrack(testLib.library(),
                              library::test::TrackSpec{.title = "Completion Track",
                                                       .artist = "Artist",
                                                       .album = "Album",
                                                       .conductor = "Conductor",
                                                       .ensemble = "Ensemble",
                                                       .soloist = "Soloist",
                                                       .uri = "/tmp/query-completion.flac",
                                                       .tags = {tags.begin(), tags.end()},
                                                       .customMetadata = {custom.begin(), custom.end()},
                                                       .duration = std::chrono::seconds{120}});
    }

    QueryExpressionCompleter makeCompleter(TestMusicLibrary& testLib,
                                           std::unique_ptr<LibraryChanges>& changesPtr,
                                           std::unique_ptr<CompletionService>& servicePtr)
    {
      changesPtr = std::make_unique<LibraryChanges>();
      servicePtr = std::make_unique<CompletionService>(testLib.library(), *changesPtr);
      return QueryExpressionCompleter{*servicePtr};
    }

    std::vector<std::string> insertTexts(std::vector<CompletionItem> const& items)
    {
      auto result = std::vector<std::string>{};

      for (auto const& item : items)
      {
        result.push_back(item.insertText);
      }

      return result;
    }
  } // namespace

  TEST_CASE("QueryExpressionCompleter - completes field aliases from query prefixes",
            "[runtime][unit][completion-query][field]")
  {
    auto testLib = TestMusicLibrary{};
    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(testLib, changesPtr, servicePtr);

    auto optAlbum = completer.complete("$al", 3);
    REQUIRE(optAlbum);
    CHECK(optAlbum->replaceBegin == 0);
    CHECK(optAlbum->replaceEnd == 3);
    CHECK(insertTexts(optAlbum->items) == std::vector<std::string>{"$album", "$albumArtist"});
    CHECK(optAlbum->items[0].detail == "alias");

    auto optTrackNumber = completer.complete("$tn", 3);
    REQUIRE(optTrackNumber);
    CHECK(insertTexts(optTrackNumber->items) == std::vector<std::string>{"$trackNumber"});

    auto optBitrate = completer.complete("@BR", 3);
    REQUIRE(optBitrate);
    CHECK(insertTexts(optBitrate->items) == std::vector<std::string>{"@bitrate"});
  }

  TEST_CASE("QueryExpressionCompleter - completes operators allowed after fields",
            "[runtime][unit][completion-query][operator]")
  {
    auto testLib = TestMusicLibrary{};
    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(testLib, changesPtr, servicePtr);

    auto optArtist = completer.complete("$artist ", 8);
    REQUIRE(optArtist);
    CHECK(optArtist->replaceBegin == 7);
    CHECK(optArtist->replaceEnd == 8);
    CHECK(insertTexts(optArtist->items) == std::vector<std::string>{" = ", " != ", " ~ ", " in ", "?"});

    auto optYear = completer.complete("$year >", 7);
    REQUIRE(optYear);
    CHECK(optYear->replaceBegin == 5);
    CHECK(optYear->replaceEnd == 7);
    CHECK(insertTexts(optYear->items) == std::vector<std::string>{" > ", " >= "});
  }

  TEST_CASE("QueryExpressionCompleter - completes logical operators after values",
            "[runtime][unit][completion-query][operator]")
  {
    auto testLib = TestMusicLibrary{};
    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(testLib, changesPtr, servicePtr);

    auto optAfterValue = completer.complete(R"($artist = "Miles" )", 18);
    REQUIRE(optAfterValue);
    CHECK(optAfterValue->replaceBegin == 17);
    CHECK(optAfterValue->replaceEnd == 18);
    CHECK(insertTexts(optAfterValue->items) == std::vector<std::string>{" and ", " or ", " && ", " || "});

    auto optAnd = completer.complete(R"($artist = "Miles" a)", 19);
    REQUIRE(optAnd);
    CHECK(optAnd->replaceBegin == 17);
    CHECK(optAnd->replaceEnd == 19);
    CHECK(insertTexts(optAnd->items) == std::vector<std::string>{" and "});

    auto optSymbol = completer.complete("$year >= 1999 |", 15);
    REQUIRE(optSymbol);
    CHECK(optSymbol->replaceBegin == 13);
    CHECK(optSymbol->replaceEnd == 15);
    CHECK(insertTexts(optSymbol->items) == std::vector<std::string>{" || "});

    auto optTag = completer.complete("#rock ", 6);
    REQUIRE(optTag);
    CHECK(optTag->replaceBegin == 5);
    CHECK(optTag->replaceEnd == 6);
    CHECK(insertTexts(optTag->items) == std::vector<std::string>{" and ", " or ", " && ", " || "});
  }

  TEST_CASE("QueryExpressionCompleter - completes metadata values for value positions",
            "[runtime][unit][completion-query][value]")
  {
    auto testLib = TestMusicLibrary{};
    auto tags = std::vector<std::string>{};
    auto custom = std::vector<std::pair<std::string, std::string>>{};
    addCompletionTrack(testLib, tags, custom);

    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(testLib, changesPtr, servicePtr);

    auto optArtist = completer.complete("$artist = Ar", 12);
    REQUIRE(optArtist);
    CHECK(optArtist->replaceBegin == 10);
    CHECK(optArtist->replaceEnd == 12);
    CHECK(optArtist->items.front().displayText == "Artist");
    CHECK(insertTexts(optArtist->items) == std::vector<std::string>{R"("Artist")"});

    auto optList = completer.complete("$artist in [Ar", 14);
    REQUIRE(optList);
    CHECK(optList->replaceBegin == 12);
    CHECK(optList->replaceEnd == 14);
    CHECK(insertTexts(optList->items) == std::vector<std::string>{R"("Artist")"});

    auto optConductor = completer.complete("$conductor = Con", 16);
    REQUIRE(optConductor);
    CHECK(optConductor->replaceBegin == 13);
    CHECK(optConductor->replaceEnd == 16);
    CHECK(insertTexts(optConductor->items) == std::vector<std::string>{R"("Conductor")"});

    CHECK_FALSE(completer.complete(R"(%"Mood" = Br)", 12));
  }

  TEST_CASE("QueryExpressionCompleter - completes tag and custom-key variables",
            "[runtime][unit][completion-query][variable]")
  {
    auto testLib = TestMusicLibrary{};
    auto tags = std::vector<std::string>{"90s Rock", "Rock"};
    auto custom = std::vector<std::pair<std::string, std::string>>{{"Replay Gain", "-6"}, {"Mood", "Bright"}};
    addCompletionTrack(testLib, tags, custom);

    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(testLib, changesPtr, servicePtr);

    auto optTag = completer.complete("#90", 3);
    REQUIRE(optTag);
    CHECK(insertTexts(optTag->items) == std::vector<std::string>{R"(#"90s Rock")"});

    auto optBareTag = completer.complete("#rock", 5);
    REQUIRE(optBareTag);
    CHECK(optBareTag->replaceBegin == 0);
    CHECK(optBareTag->replaceEnd == 5);
    CHECK(insertTexts(optBareTag->items) == std::vector<std::string>{"#Rock"});

    auto optCustomKey = completer.complete("%Replay", 7);
    REQUIRE(optCustomKey);
    CHECK(insertTexts(optCustomKey->items) == std::vector<std::string>{R"(%"Replay Gain")"});
  }

  TEST_CASE("QueryExpressionCompleter - respects limits and token boundaries",
            "[runtime][unit][completion-query][boundary]")
  {
    auto testLib = TestMusicLibrary{};
    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(testLib, changesPtr, servicePtr);

    auto optLimited = completer.complete("$", 1, 2);
    REQUIRE(optLimited);
    CHECK(optLimited->items.size() == 2);
    CHECK(insertTexts(optLimited->items) == std::vector<std::string>{"$title", "$artist"});

    CHECK_FALSE(completer.complete(R"("$artist")", 5));
    CHECK_FALSE(completer.complete("$artist", 3));
    CHECK_FALSE(completer.complete("$", 1, 0));
  }
} // namespace ao::rt::test
