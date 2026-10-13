// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("LibrarySnapshot - custom metadata values read owning per-key state under one transaction",
            "[runtime][unit][library][readmodel]")
  {
    auto tempDir = ao::test::TempDir{};
    auto storage = library::test::makeTestMusicLibrary(tempDir.path(), LibraryPaths{tempDir.path()}.databasePath());
    auto const trackId = library::test::addTrackWithUniqueFixtureUri(
      storage, {.title = "Song", .uri = "song.flac", .customMetadata = {{"Mood", "Old"}, {"Notes", ""}}});
    auto executor = QueuedExecutor{};
    auto changes = makeLibraryChanges(executor, storage);
    auto commands = LibraryCommandsFixture{storage, changes, executor};
    auto& library = commands.library();

    SECTION("the owning value outlives the snapshot and its read transaction")
    {
      auto optValue = std::optional<std::string>{};

      {
        auto snapshot = library.snapshot();
        optValue = snapshot.trackCustomMetadataValue(trackId, "Mood");
        REQUIRE(optValue);
        CHECK(*optValue == "Old");
      }

      // The snapshot and its transaction are gone; the string is owning.
      CHECK(*optValue == "Old");
    }

    SECTION("an empty stored value stays engaged while missing keys and tracks read absent")
    {
      auto snapshot = library.snapshot();

      auto const optEmpty = snapshot.trackCustomMetadataValue(trackId, "Notes");
      REQUIRE(optEmpty);
      CHECK(optEmpty->empty());
      CHECK_FALSE(snapshot.trackCustomMetadataValue(trackId, "Absent").has_value());
      CHECK_FALSE(snapshot.trackCustomMetadataValue(TrackId{999999}, "Mood").has_value());
    }

    SECTION("a captured revision keeps the old value while a newer snapshot sees the writer's update")
    {
      auto oldSnapshot = library.snapshot();
      auto const optBefore = oldSnapshot.trackCustomMetadataValue(trackId, "Mood");
      REQUIRE(optBefore);
      CHECK(*optBefore == "Old");

      auto patch = MetadataPatch{};
      patch.customUpdates["Mood"] = std::string{"New"};
      REQUIRE(commands.updateMetadata(std::array{trackId}, patch));

      auto const optRetained = oldSnapshot.trackCustomMetadataValue(trackId, "Mood");
      REQUIRE(optRetained);
      CHECK(*optRetained == "Old");

      auto fresh = library.snapshot();
      CHECK(fresh.revision() == oldSnapshot.revision() + 1);
      auto const optUpdated = fresh.trackCustomMetadataValue(trackId, "Mood");
      REQUIRE(optUpdated);
      CHECK(*optUpdated == "New");
    }

    SECTION("missing lookups do not mutate the dictionary")
    {
      auto const sizeBefore = storage.dictionary().size();
      auto const generationBefore = storage.dictionary().generation();

      {
        auto snapshot = library.snapshot();
        auto const revisionBefore = snapshot.revision();
        CHECK_FALSE(snapshot.trackCustomMetadataValue(trackId, "Absent").has_value());
        CHECK_FALSE(snapshot.trackCustomMetadataValue(TrackId{999999}, "Mood").has_value());
        CHECK_FALSE(snapshot.trackCustomMetadataValue(TrackId{999999}, "Never interned key").has_value());
        CHECK(snapshot.revision() == revisionBefore);
      }

      CHECK(storage.dictionary().size() == sizeBefore);
      CHECK(storage.dictionary().generation() == generationBefore);
    }
  }
} // namespace ao::rt::test
