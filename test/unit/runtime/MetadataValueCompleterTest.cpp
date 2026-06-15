// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtils.h"
#include <ao/Type.h>
#include <ao/async/Runtime.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/CompletionItem.h>
#include <ao/rt/CompletionService.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/MetadataValueCompleter.h>
#include <ao/rt/TrackField.h>

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
      auto txn = testLib.library().writeTransaction();
      auto writer = testLib.library().tracks().writer(txn);
      auto builder = library::TrackBuilder::createNew();

      builder.metadata().title("Metadata Value Track").artist(std::move(artist)).album(std::move(album));
      builder.property()
        .uri("/tmp/metadata-value-completion.flac")
        .duration(std::chrono::seconds{120})
        .bitrate(Bitrate{320000})
        .sampleRate(SampleRate{44100})
        .channels(Channels{2})
        .bitDepth(BitDepth{16});

      auto hotData = builder.serializeHot(txn, testLib.library().dictionary());
      auto coldData = builder.serializeCold(txn, testLib.library().dictionary(), testLib.library().resources());
      writer.createHotCold(hotData, coldData);
      txn.commit();
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

  TEST_CASE("MetadataValueCompleter - completes supported field values", "[runtime][unit][completion]")
  {
    auto testLib = TestMusicLibrary{};
    addMetadataValueTrack(testLib, "Massive Attack", "Mezzanine");
    addMetadataValueTrack(testLib, "Massive Attack", "Protection");
    addMetadataValueTrack(testLib, "Mazzy Star", "So Tonight That I Might See");

    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutation = LibraryMutationService{runtime, testLib.library()};
    auto service = CompletionService{testLib.library(), mutation};

    auto artistCompleter = MetadataValueCompleter{service, TrackField::Artist};
    auto artistItems = artistCompleter.complete("ma");

    CHECK(insertTexts(artistItems) == std::vector<std::string>{"Massive Attack", "Mazzy Star"});
    REQUIRE_FALSE(artistItems.empty());
    CHECK(artistItems[0].displayText == "Massive Attack");
    CHECK(artistItems[0].detail == "2");

    auto albumCompleter = MetadataValueCompleter{service, TrackField::Album};
    CHECK(insertTexts(albumCompleter.complete("pro")) == std::vector<std::string>{"Protection"});
  }

  TEST_CASE("MetadataValueCompleter - rejects unsupported fields and limits results", "[runtime][unit][completion]")
  {
    auto testLib = TestMusicLibrary{};
    addMetadataValueTrack(testLib, "Artist A", "Album A");
    addMetadataValueTrack(testLib, "Artist B", "Album B");

    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutation = LibraryMutationService{runtime, testLib.library()};
    auto service = CompletionService{testLib.library(), mutation};

    auto titleCompleter = MetadataValueCompleter{service, TrackField::Title};
    CHECK(titleCompleter.complete("Metadata").empty());

    auto artistCompleter = MetadataValueCompleter{service, TrackField::Artist};
    CHECK(insertTexts(artistCompleter.complete("artist", 1)) == std::vector<std::string>{"Artist A"});
    CHECK(artistCompleter.complete("artist", 0).empty());
  }

  TEST_CASE("MetadataValueCompleter - adapts entry text to whole-value replacement", "[runtime][unit][completion]")
  {
    auto testLib = TestMusicLibrary{};
    addMetadataValueTrack(testLib, "Massive Attack", "Mezzanine");
    addMetadataValueTrack(testLib, "Mazzy Star", "She Hangs Brightly");

    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutation = LibraryMutationService{runtime, testLib.library()};
    auto service = CompletionService{testLib.library(), mutation};

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
