// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/completion/MetadataValueCompleter.h>

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/i18n/IcuCompletionAliases.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionService.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void addMetadataValueTrack(MusicLibraryFixture& libraryFixture, std::string artist, std::string album)
    {
      library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
                                                  library::test::TrackSpec{.title = "Metadata Value Track",
                                                                           .artist = std::move(artist),
                                                                           .album = std::move(album),
                                                                           .uri = "metadata-value-completion.flac",
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
    auto libraryFixture = MusicLibraryFixture{};
    addMetadataValueTrack(libraryFixture, "Massive Attack", "Mezzanine");
    addMetadataValueTrack(libraryFixture, "Massive Attack", "Protection");
    addMetadataValueTrack(libraryFixture, "Mazzy Star", "So Tonight That I Might See");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto service = CompletionService{libraryFixture.library(), changes};

    auto artistCompleter = MetadataValueCompleter{service, TrackField::Artist};
    auto artistItems = artistCompleter.complete("ma");

    CHECK(insertTexts(artistItems) == std::vector<std::string>{"Massive Attack", "Mazzy Star"});
    REQUIRE_FALSE(artistItems.empty());
    CHECK(artistItems[0].displayText == "Massive Attack");
    CHECK(artistItems[0].detail.kind == CompletionDetailKind::Frequency);
    CHECK(artistItems[0].detail.frequency == 2);

    auto albumCompleter = MetadataValueCompleter{service, TrackField::Album};
    CHECK(insertTexts(albumCompleter.complete("pro")) == std::vector<std::string>{"Protection"});
  }

  TEST_CASE("MetadataValueCompleter - rejects unsupported fields and limits results",
            "[runtime][unit][completion-value][limit]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    addMetadataValueTrack(libraryFixture, "Artist A", "Album A");
    addMetadataValueTrack(libraryFixture, "Artist B", "Album B");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto service = CompletionService{libraryFixture.library(), changes};

    auto titleCompleter = MetadataValueCompleter{service, TrackField::Title};
    CHECK(titleCompleter.complete("Metadata").empty());

    auto artistCompleter = MetadataValueCompleter{service, TrackField::Artist};
    CHECK(insertTexts(artistCompleter.complete("artist", 1)) == std::vector<std::string>{"Artist A"});
    CHECK(artistCompleter.complete("artist", 0).empty());
  }

  TEST_CASE("MetadataValueCompleter - whole-value prefixes outrank interior word prefixes",
            "[runtime][unit][completion-value][ranking]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    addMetadataValueTrack(libraryFixture, "Trevor Pinnock", "One");
    addMetadataValueTrack(libraryFixture, "Trevor Pinnock", "Two");
    addMetadataValueTrack(libraryFixture, "Pinnock Ensemble", "Three");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto service = CompletionService{libraryFixture.library(), changes};
    auto completer = MetadataValueCompleter{service, TrackField::Artist};

    CHECK(insertTexts(completer.complete("pinn")) == std::vector<std::string>{"Pinnock Ensemble", "Trevor Pinnock"});
    CHECK(insertTexts(completer.complete("PINNOCK", 1)) == std::vector<std::string>{"Pinnock Ensemble"});
    CHECK(completer.complete("innock").empty());
    CHECK(completer.complete("pinnok").empty());
  }

  TEST_CASE("MetadataValueCompleter - adapts entry text to whole-value replacement",
            "[runtime][unit][completion-value][provider]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    addMetadataValueTrack(libraryFixture, "Massive Attack", "Mezzanine");
    addMetadataValueTrack(libraryFixture, "Mazzy Star", "She Hangs Brightly");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto service = CompletionService{libraryFixture.library(), changes};

    auto provider = MetadataValueCompleter{service, TrackField::Artist}.asProvider();
    auto optResult = provider("ma suffix", 2);

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 0);
    CHECK(optResult->replaceEnd == std::string{"ma suffix"}.size());
    CHECK(insertTexts(optResult->items) == std::vector<std::string>{"Massive Attack", "Mazzy Star"});

    auto unsupportedProvider = MetadataValueCompleter{service, TrackField::Title}.asProvider();
    CHECK_FALSE(unsupportedProvider("Metadata", 3));
  }

  TEST_CASE("MetadataValueCompleter - alias matches preserve source text and rank below direct matches",
            "[runtime][unit][completion-alias][value]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    addMetadataValueTrack(libraryFixture, "周杰倫", "One");
    addMetadataValueTrack(libraryFixture, "周杰倫", "Two");
    addMetadataValueTrack(libraryFixture, "Zhou Direct", "Three");
    addMetadataValueTrack(libraryFixture, "hanハンバート", "Four");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto aliasPolicyPtr = i18n::createIcuCompletionAliasPolicy();
    auto service = CompletionService{libraryFixture.library(), changes, nullptr, aliasPolicyPtr.get()};
    auto completer = MetadataValueCompleter{service, TrackField::Artist};

    CHECK(insertTexts(completer.complete("zhoujielun")) == std::vector<std::string>{"周杰倫"});
    CHECK(insertTexts(completer.complete("zhou", 1)) == std::vector<std::string>{"Zhou Direct"});
    CHECK(insertTexts(completer.complete("han")) == std::vector<std::string>{"hanハンバート"});
    CHECK(completer.complete("周abc").empty());
  }
} // namespace ao::rt::test
