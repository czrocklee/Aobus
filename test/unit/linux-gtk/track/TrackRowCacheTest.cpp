// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackRowCache.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/refptr.h>
#include <gtkmm/application.h>

#include <chrono>
#include <string>

namespace ao::gtk::test
{
  TEST_CASE("TrackRowCache - loads cached rows from runtime track data", "[gtk][unit][track][row-cache]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.row_cache_test");
    auto basicId1 = kInvalidTrackId;
    auto basicId2 = kInvalidTrackId;
    auto utf8Id = kInvalidTrackId;
    auto helperId = kInvalidTrackId;
    auto cachingId = kInvalidTrackId;
    auto invalidationId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                                     {
                                       auto spec1 = library::test::TrackSpec{};
                                       spec1.artist = "Artist 1";
                                       spec1.album = "Album 1";
                                       spec1.title = "Track 1";
                                       spec1.genre = "Genre 1";
                                       spec1.uri = "track-1.flac";
                                       spec1.year = 2021;
                                       spec1.trackNumber = 1;
                                       spec1.duration = std::chrono::minutes{3};
                                       basicId1 = library::test::addTrackWithUniqueFixtureUri(musicLibrary, spec1);

                                       auto spec2 = library::test::TrackSpec{};
                                       spec2.title = "Track 2";
                                       spec2.uri = "track-2.flac";
                                       spec2.duration = std::chrono::minutes{4};
                                       basicId2 = library::test::addTrackWithUniqueFixtureUri(musicLibrary, spec2);

                                       auto utf8Spec = library::test::TrackSpec{};
                                       utf8Spec.title = "東京の歌";
                                       utf8Spec.artist = "Björk";
                                       utf8Spec.album = "Álbum del Niño";
                                       utf8Spec.albumArtist = "Sigur Rós";
                                       utf8Spec.genre = "Électronique";
                                       utf8Spec.composer = "久石譲";
                                       utf8Spec.conductor = "指揮者";
                                       utf8Spec.ensemble = "東京交響楽団";
                                       utf8Spec.work = "作品一";
                                       utf8Spec.movement = "第一楽章";
                                       utf8Spec.soloist = "独奏者";
                                       utf8Spec.uri = "utf8.flac";
                                       utf8Spec.tags = {"夜", "ライブ"};
                                       utf8Id = library::test::addTrackWithUniqueFixtureUri(musicLibrary, utf8Spec);

                                       auto helperSpec = library::test::TrackSpec{};
                                       helperSpec.uri = "test.flac";
                                       helperSpec.duration = std::chrono::minutes{2};
                                       helperId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, helperSpec);
                                       cachingId = library::test::addTrackWithUniqueFixtureUri(
                                         musicLibrary, library::test::TrackSpec{.uri = "cache.flac"});
                                       invalidationId = library::test::addTrackWithUniqueFixtureUri(
                                         musicLibrary, library::test::TrackSpec{.uri = "invalidation.flac"});
                                     }};
    auto& runtime = fixture.runtime();

    SECTION("Basic data loading")
    {
      auto provider = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

      Glib::RefPtr<TrackRowObject> const row1Ptr = provider.trackRow(basicId1);
      REQUIRE(row1Ptr);
      CHECK(row1Ptr->fieldText(rt::TrackField::Artist) == "Artist 1");
      CHECK(row1Ptr->fieldText(rt::TrackField::Album) == "Album 1");
      CHECK(row1Ptr->fieldText(rt::TrackField::Title) == "Track 1");
      CHECK(row1Ptr->fieldText(rt::TrackField::Genre) == "Genre 1");
      CHECK(row1Ptr->year() == 2021);
      CHECK(row1Ptr->trackNumber() == 1);
      CHECK(row1Ptr->duration() == std::chrono::minutes{3});

      auto const row2Ptr = provider.trackRow(basicId2);
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
      auto provider = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

      auto const rowPtr = provider.trackRow(utf8Id);
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

    SECTION("Clearing the cache discards loaded rows")
    {
      auto provider = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

      auto const rowBeforeClearPtr = provider.trackRow(helperId);
      REQUIRE(rowBeforeClearPtr);

      provider.clearCache();

      auto const rowAfterClearPtr = provider.trackRow(helperId);
      REQUIRE(rowAfterClearPtr);
      CHECK(rowAfterClearPtr != rowBeforeClearPtr);
    }

    // FilePath is a text-backed value materialized from the read-model row.
    SECTION("File path is materialized into the row")
    {
      auto provider = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

      auto const rowPtr = provider.trackRow(helperId);
      REQUIRE(rowPtr);

      auto const expected = Glib::ustring{(runtime.musicLibrary().rootPath() / "test.flac").string()};
      REQUIRE(rowPtr->stringField(rt::TrackField::FilePath) != nullptr);
      CHECK(*rowPtr->stringField(rt::TrackField::FilePath) == expected);
      CHECK(rowPtr->fieldText(rt::TrackField::FilePath) == expected);
    }

    SECTION("Caching works")
    {
      auto provider = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

      auto const row1APtr = provider.trackRow(cachingId);
      auto const row1BPtr = provider.trackRow(cachingId);

      REQUIRE(row1APtr);
      REQUIRE(row1BPtr);
      CHECK(row1APtr == row1BPtr);
    }

    SECTION("Invalidation")
    {
      auto provider = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

      auto const row1Ptr = provider.trackRow(invalidationId);
      CHECK(row1Ptr);
      provider.invalidate(invalidationId);

      auto const row1NewPtr = provider.trackRow(invalidationId);
      CHECK(row1Ptr != row1NewPtr);
    }

    SECTION("Non-existent track")
    {
      auto provider = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
      auto const rowPtr = provider.trackRow(TrackId{999});
      CHECK_FALSE(rowPtr);
    }
  }

  TEST_CASE("TrackRowCache - a library mutation invalidates the cached row it changed",
            "[gtk][regression][track][row-cache]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.row_cache_invalidation_test");
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

    // The cache keeps itself coherent, so nothing above it has to hold a
    // library subscription or remember to invalidate after a mutation.
    auto const trackId = addRuntimeTrack(runtime, {.title = "Before Import"});
    auto const rowBeforePtr = cache.trackRow(trackId);
    REQUIRE(rowBeforePtr);
    CHECK(rowBeforePtr->fieldText(rt::TrackField::Title) == "Before Import");

    updateRuntimeTrack(runtime, trackId, [](library::test::TrackSpec& spec) { spec.title = "After Import"; });

    auto const rowAfterPtr = cache.trackRow(trackId);
    REQUIRE(rowAfterPtr);
    CHECK(rowAfterPtr->fieldText(rt::TrackField::Title) == "After Import");
  }
} // namespace ao::gtk::test
