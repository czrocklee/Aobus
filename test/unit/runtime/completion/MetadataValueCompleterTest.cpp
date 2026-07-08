// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void addMetadataValueTrack(TestMusicLibrary& testLib, std::string artist, std::string album)
    {
      library::test::addTrack(testLib.library(),
                              library::test::TrackSpec{.title = "Metadata Value Track",
                                                       .artist = std::move(artist),
                                                       .album = std::move(album),
                                                       .uri = "/tmp/metadata-value-completion.flac",
                                                       .duration = std::chrono::seconds{120}});
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

  TEST_CASE("MetadataValueCompleter - completes supported field values", "[runtime][unit][completion][value]")
  {
    auto testLib = TestMusicLibrary{};
    addMetadataValueTrack(testLib, "Massive Attack", "Mezzanine");
    addMetadataValueTrack(testLib, "Massive Attack", "Protection");
    addMetadataValueTrack(testLib, "Mazzy Star", "So Tonight That I Might See");

    auto changes = LibraryChanges{};
    auto service = CompletionService{testLib.library(), changes};

    auto artistCompleter = MetadataValueCompleter{service, TrackField::Artist};
    auto artistItems = artistCompleter.complete("ma");

    CHECK(insertTexts(artistItems) == std::vector<std::string>{"Massive Attack", "Mazzy Star"});
    REQUIRE_FALSE(artistItems.empty());
    CHECK(artistItems[0].displayText == "Massive Attack");
    CHECK(artistItems[0].detail == "2");

    auto albumCompleter = MetadataValueCompleter{service, TrackField::Album};
    CHECK(insertTexts(albumCompleter.complete("pro")) == std::vector<std::string>{"Protection"});
  }

  TEST_CASE("MetadataValueCompleter - rejects unsupported fields and limits results",
            "[runtime][unit][completion-value][limit]")
  {
    auto testLib = TestMusicLibrary{};
    addMetadataValueTrack(testLib, "Artist A", "Album A");
    addMetadataValueTrack(testLib, "Artist B", "Album B");

    auto changes = LibraryChanges{};
    auto service = CompletionService{testLib.library(), changes};

    auto titleCompleter = MetadataValueCompleter{service, TrackField::Title};
    CHECK(titleCompleter.complete("Metadata").empty());

    auto artistCompleter = MetadataValueCompleter{service, TrackField::Artist};
    CHECK(insertTexts(artistCompleter.complete("artist", 1)) == std::vector<std::string>{"Artist A"});
    CHECK(artistCompleter.complete("artist", 0).empty());
  }

  TEST_CASE("MetadataValueCompleter - adapts entry text to whole-value replacement",
            "[runtime][unit][completion-value][provider]")
  {
    auto testLib = TestMusicLibrary{};
    addMetadataValueTrack(testLib, "Massive Attack", "Mezzanine");
    addMetadataValueTrack(testLib, "Mazzy Star", "She Hangs Brightly");

    auto changes = LibraryChanges{};
    auto service = CompletionService{testLib.library(), changes};

    auto provider = MetadataValueCompleter{service, TrackField::Artist}.asProvider();
    auto optResult = provider("ma suffix", 2);

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 0);
    CHECK(optResult->replaceEnd == std::string{"ma suffix"}.size());
    CHECK(insertTexts(optResult->items) == std::vector<std::string>{"Massive Attack", "Mazzy Star"});

    auto unsupportedProvider = MetadataValueCompleter{service, TrackField::Title}.asProvider();
    CHECK_FALSE(unsupportedProvider("Metadata", 3));
  }
} // namespace ao::rt::test
