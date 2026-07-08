// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackRowCache.h"

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/refptr.h>
#include <gtkmm/application.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TrackRowCache - loads cached rows from runtime track data", "[gtk][unit][track][row-cache]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.row_cache_test");
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& musicLibrary = runtime.musicLibrary();

    SECTION("Basic data loading")
    {
      auto spec1 = library::test::TrackSpec{};
      spec1.artist = "Artist 1";
      spec1.album = "Album 1";
      spec1.title = "Track 1";
      spec1.genre = "Genre 1";
      spec1.year = 2021;
      spec1.trackNumber = 1;
      spec1.duration = std::chrono::minutes{3};

      auto spec2 = library::test::TrackSpec{};
      spec2.title = "Track 2";
      spec2.duration = std::chrono::minutes{4};

      auto const id1 = library::test::addTrack(musicLibrary, spec1);
      auto const id2 = library::test::addTrack(musicLibrary, spec2);

      auto provider = TrackRowCache{runtime.library()};

      auto const row1Ptr = provider.trackRow(id1);
      REQUIRE(row1Ptr);
      CHECK(row1Ptr->fieldText(rt::TrackField::Artist) == "Artist 1");
      CHECK(row1Ptr->fieldText(rt::TrackField::Album) == "Album 1");
      CHECK(row1Ptr->fieldText(rt::TrackField::Title) == "Track 1");
      CHECK(row1Ptr->fieldText(rt::TrackField::Genre) == "Genre 1");
      CHECK(row1Ptr->year() == 2021);
      CHECK(row1Ptr->trackNumber() == 1);
      CHECK(row1Ptr->duration() == std::chrono::minutes{3});

      auto const row2Ptr = provider.trackRow(id2);
      REQUIRE(row2Ptr);
      CHECK(row2Ptr->fieldText(rt::TrackField::Title) == "Track 2");
      CHECK(row2Ptr->duration() == std::chrono::minutes{4});

      // Verify the playing flag setter/getter
      CHECK_FALSE(row1Ptr->isPlaying());
      row1Ptr->setPlaying(true);
      CHECK(row1Ptr->isPlaying());
      row1Ptr->setPlaying(false);
      CHECK_FALSE(row1Ptr->isPlaying());

      // Verify custom string fields and failure paths
      CHECK(row1Ptr->setStringField(rt::TrackField::Artist, "New Artist"));
      CHECK(row1Ptr->fieldText(rt::TrackField::Artist) == "New Artist");
      CHECK_FALSE(row1Ptr->setStringField(rt::TrackField::Duration, "Failed"));

      // Verify other metadata and resource/playback properties
      row1Ptr->setYear(2025);
      row1Ptr->setDiscNumber(2);
      row1Ptr->setDiscTotal(3);
      row1Ptr->setTrackNumber(4);
      row1Ptr->setTrackTotal(10);
      CHECK(row1Ptr->year() == 2025);
      CHECK(row1Ptr->discNumber() == 2);
      CHECK(row1Ptr->discTotal() == 3);
      CHECK(row1Ptr->trackNumber() == 4);
      CHECK(row1Ptr->trackTotal() == 10);
      CHECK(row1Ptr->sampleRate() == 44100);
      CHECK(row1Ptr->channels() == 2);
      CHECK(row1Ptr->bitDepth() == 16);

      // displayText() memoizes computed fields and must drop the cached string
      // when a contributing setter runs. Year was set to 2025 above; read it once
      // (fills the cache), then mutate and confirm the refreshed value, not stale.
      REQUIRE(row1Ptr->displayText(rt::TrackField::Year) != nullptr);
      CHECK(*row1Ptr->displayText(rt::TrackField::Year) == "2025");
      row1Ptr->setYear(1999);
      CHECK(*row1Ptr->displayText(rt::TrackField::Year) == "1999");

      // The TrackNumber setter ran above (value 4); a first-time computed read
      // must still format correctly from scratch (lazy fill).
      CHECK(*row1Ptr->displayText(rt::TrackField::TrackNumber) == "4");

      // Text-backed fields share the same stored slot as stringField() — no
      // separate cache, so displayText() returns the identical pointer.
      REQUIRE(row1Ptr->displayText(rt::TrackField::Artist) != nullptr);
      CHECK(row1Ptr->displayText(rt::TrackField::Artist) == row1Ptr->stringField(rt::TrackField::Artist));
      CHECK(*row1Ptr->displayText(rt::TrackField::Artist) == "New Artist");
      CHECK(row1Ptr->displayText(static_cast<rt::TrackField>(255)) == nullptr);
    }

    SECTION("UTF-8 metadata survives row materialization")
    {
      auto spec = library::test::TrackSpec{};
      spec.title = "東京の歌";
      spec.artist = "Björk";
      spec.album = "Álbum del Niño";
      spec.albumArtist = "Sigur Rós";
      spec.genre = "Électronique";
      spec.composer = "久石譲";
      spec.conductor = "指揮者";
      spec.ensemble = "東京交響楽団";
      spec.work = "作品一";
      spec.movement = "第一楽章";
      spec.soloist = "独奏者";
      spec.tags = {"夜", "ライブ"};

      auto const id = library::test::addTrack(musicLibrary, spec);
      auto provider = TrackRowCache{runtime.library()};

      auto const rowPtr = provider.trackRow(id);
      REQUIRE(rowPtr);

      CHECK(rowPtr->fieldText(rt::TrackField::Title) == "東京の歌");
      CHECK(rowPtr->fieldText(rt::TrackField::Artist) == "Björk");
      CHECK(rowPtr->fieldText(rt::TrackField::Album) == "Álbum del Niño");
      CHECK(rowPtr->fieldText(rt::TrackField::AlbumArtist) == "Sigur Rós");
      CHECK(rowPtr->fieldText(rt::TrackField::Genre) == "Électronique");
      CHECK(rowPtr->fieldText(rt::TrackField::Composer) == "久石譲");
      CHECK(rowPtr->fieldText(rt::TrackField::Conductor) == "指揮者");
      CHECK(rowPtr->fieldText(rt::TrackField::Ensemble) == "東京交響楽団");
      CHECK(rowPtr->fieldText(rt::TrackField::Work) == "作品一");
      CHECK(rowPtr->fieldText(rt::TrackField::Movement) == "第一楽章");
      CHECK(rowPtr->fieldText(rt::TrackField::Soloist) == "独奏者");
      CHECK(rowPtr->tags() == "夜, ライブ");
    }

    SECTION("Cache helper methods")
    {
      auto spec = library::test::TrackSpec{};
      spec.duration = std::chrono::minutes{2};
      auto const id = library::test::addTrack(musicLibrary, spec);

      auto provider = TrackRowCache{runtime.library()};

      auto const optUri = provider.uriPath(id);
      REQUIRE(optUri);
      CHECK(optUri->string() == "/tmp/test.flac");

      CHECK(provider.coverArtId(id) == kInvalidResourceId);

      auto const rowBeforeClearPtr = provider.trackRow(id);
      REQUIRE(rowBeforeClearPtr);

      provider.clearCache();

      auto const rowAfterClearPtr = provider.trackRow(id);
      REQUIRE(rowAfterClearPtr);
      CHECK(rowAfterClearPtr != rowBeforeClearPtr);

      provider.remove(id);

      auto const rowAfterRemovePtr = provider.trackRow(id);
      REQUIRE(rowAfterRemovePtr);
      CHECK(rowAfterRemovePtr != rowAfterClearPtr);
    }

    SECTION("Caching works")
    {
      auto const id1 = library::test::addTrack(musicLibrary, {});
      auto provider = TrackRowCache{runtime.library()};

      auto const row1APtr = provider.trackRow(id1);
      auto const row1BPtr = provider.trackRow(id1);

      REQUIRE(row1APtr);
      REQUIRE(row1BPtr);
      CHECK(row1APtr == row1BPtr);
    }

    SECTION("Invalidation")
    {
      auto const id1 = library::test::addTrack(musicLibrary, {});
      auto provider = TrackRowCache{runtime.library()};

      auto const row1Ptr = provider.trackRow(id1);
      CHECK(row1Ptr);
      provider.invalidate(id1);

      auto const row1NewPtr = provider.trackRow(id1);
      CHECK(row1Ptr != row1NewPtr);
    }

    SECTION("Dictionary resolution")
    {
      library::test::addTrack(musicLibrary, library::test::TrackSpec{.title = "Test Dictionary String"});
      auto provider = TrackRowCache{runtime.library()};
      auto const id = DictionaryId{1}; // Assuming ID 1 exists because it's the first string added to the dict

      auto const& name = provider.resolveDictionaryString(id);
      CHECK_FALSE(name.empty());

      provider.clearCache();
    }

    SECTION("Non-existent track")
    {
      auto provider = TrackRowCache{runtime.library()};
      auto const rowPtr = provider.trackRow(TrackId{999});
      CHECK_FALSE(rowPtr);
    }
  }
} // namespace ao::gtk::test
