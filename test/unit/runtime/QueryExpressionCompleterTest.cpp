// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/Type.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
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
    TrackId addCompletionTrack(TestMusicLibrary& testLib,
                               std::span<std::string const> tags,
                               std::span<std::pair<std::string, std::string> const> custom)
    {
      auto txn = testLib.library().writeTransaction();
      auto writer = testLib.library().tracks().writer(txn);
      auto builder = library::TrackBuilder::createNew();

      builder.metadata().title("Completion Track").artist("Artist").album("Album");
      builder.property()
        .uri("/tmp/query-completion.flac")
        .duration(std::chrono::seconds{120})
        .bitrate(Bitrate{320000})
        .sampleRate(SampleRate{44100})
        .channels(Channels{2})
        .bitDepth(BitDepth{16});

      for (auto const& tag : tags)
      {
        builder.tags().add(tag);
      }

      for (auto const& [key, value] : custom)
      {
        builder.customMetadata().add(key, value);
      }

      auto hotData = builder.serializeHot(txn, testLib.library().dictionary());
      REQUIRE(hotData);
      auto coldData = builder.serializeCold(txn, testLib.library().dictionary(), testLib.library().resources());
      REQUIRE(coldData);
      auto [id, _] = ao::test::requireValue(writer.createHotCold(*hotData, *coldData));
      txn.commit();
      return id;
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

  TEST_CASE("QueryExpressionCompleter - Completes Query Fields", "[runtime][unit][completion]")
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

  TEST_CASE("QueryExpressionCompleter - Completes Query Operators", "[runtime][unit][completion]")
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

  TEST_CASE("QueryExpressionCompleter - Completes Query Logical Operators", "[runtime][unit][completion]")
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

  TEST_CASE("QueryExpressionCompleter - Completes Query Values", "[runtime][unit][completion]")
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

    CHECK_FALSE(completer.complete(R"(%"Mood" = Br)", 12));
  }

  TEST_CASE("QueryExpressionCompleter - Completes Query User Variables", "[runtime][unit][completion]")
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

  TEST_CASE("QueryExpressionCompleter - Respects Limits And Token Boundaries", "[runtime][unit][completion]")
  {
    auto testLib = TestMusicLibrary{};
    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(testLib, changesPtr, servicePtr);

    auto optLimited = completer.complete("$", 1, 2);
    REQUIRE(optLimited);
    REQUIRE(optLimited->items.size() == 2);
    CHECK(insertTexts(optLimited->items) == std::vector<std::string>{"$title", "$artist"});

    CHECK_FALSE(completer.complete(R"("$artist")", 5));
    CHECK_FALSE(completer.complete("$artist", 3));
    CHECK_FALSE(completer.complete("$", 1, 0));
  }
} // namespace ao::rt::test
