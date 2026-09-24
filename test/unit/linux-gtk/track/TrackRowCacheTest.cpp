// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackRowCache.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "track/TrackRowObject.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    constexpr auto kBindFields = std::array{
      rt::TrackField::Title,
      rt::TrackField::Artist,
      rt::TrackField::Album,
      rt::TrackField::AlbumArtist,
      rt::TrackField::Genre,
      rt::TrackField::Year,
      rt::TrackField::Duration,
      rt::TrackField::TrackNumber,
      rt::TrackField::Bitrate,
      rt::TrackField::SampleRate,
    };

    std::vector<TrackId> seedLibrary(library::MusicLibrary& library, std::size_t count)
    {
      auto ids = std::vector<TrackId>{};
      ids.reserve(count);
      auto transaction = library::test::writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();
          for (std::size_t const i : std::views::iota(std::size_t{0}, count))
          {
            auto const title = std::format("Track {}", i);
            auto const artist = std::format("Artist {}", i % 500);
            auto const album = std::format("Album {}", i % 1000);
            auto const albumArtist = std::format("AlbumArtist {}", i % 400);
            auto const genre = std::format("Genre {}", i % 40);
            auto const uri = std::format("music/track_{}.flac", i);
            auto builder = library::TrackBuilder::makeEmpty();
            builder.metadata()
              .title(title)
              .artist(artist)
              .album(album)
              .albumArtist(albumArtist)
              .genre(genre)
              .year(static_cast<std::uint16_t>(1950 + (i % 70)))
              .trackNumber(static_cast<std::uint16_t>(1 + (i % 30)));
            builder.property()
              .uri(uri)
              .duration(std::chrono::milliseconds{120000 + static_cast<std::ptrdiff_t>(i % 200000)})
              .bitrate(Bitrate{320000})
              .sampleRate(SampleRate{44100})
              .channels(Channels{2})
              .bitDepth(BitDepth{16});
            ids.push_back(ao::test::requireValue(writer.create(builder, library::FileManifestBuilder::makeEmpty())));
          }
          return {};
        }));
      REQUIRE(transaction.commit());
      return ids;
    }
  } // namespace

  TEST_CASE("TrackRowCache - loads cached rows from runtime track data", "[gtk][unit][track][row-cache]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
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

      CHECK_FALSE(row1Ptr->isPlaying());
      CHECK(row1Ptr->sampleRate() == 44100);
      CHECK(row1Ptr->channels() == 2);
      CHECK(row1Ptr->bitDepth() == 16);
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

    SECTION("Lookup retains rows until selective invalidation or whole-cache clearing")
    {
      auto provider = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
      auto const ids = std::array{helperId, cachingId, invalidationId};
      auto rows = std::array<Glib::RefPtr<TrackRowObject>, 3>{};
      CHECK(provider.cachedRowCount() == 0);

      for (std::size_t index = 0; index < ids.size(); ++index)
      {
        rows[index] = provider.trackRow(ids[index]);
        REQUIRE(rows[index]);
        CHECK(rows[index]->trackId() == ids[index]);
        CHECK(provider.cachedRowCount() == index + 1);
        CHECK(provider.trackRow(ids[index]) == rows[index]);
        CHECK(provider.cachedRowCount() == index + 1);
      }

      provider.invalidate(invalidationId);
      CHECK(provider.cachedRowCount() == 2);
      CHECK(provider.trackRow(helperId) == rows[0]);
      CHECK(provider.trackRow(cachingId) == rows[1]);
      auto const reloadedInvalidatedRowPtr = provider.trackRow(invalidationId);
      REQUIRE(reloadedInvalidatedRowPtr);
      CHECK(reloadedInvalidatedRowPtr->trackId() == invalidationId);
      CHECK(reloadedInvalidatedRowPtr != rows[2]);
      CHECK(provider.cachedRowCount() == 3);

      auto const rowsBeforeClear = std::array{rows[0], rows[1], reloadedInvalidatedRowPtr};
      provider.clearCache();
      CHECK(provider.cachedRowCount() == 0);

      for (std::size_t index = 0; index < ids.size(); ++index)
      {
        auto const rowPtr = provider.trackRow(ids[index]);
        REQUIRE(rowPtr);
        CHECK(rowPtr->trackId() == ids[index]);
        CHECK(rowPtr != rowsBeforeClear[index]);
        CHECK(provider.cachedRowCount() == index + 1);
      }
    }

    // FilePath is a text-backed value materialized from the read-model row.
    SECTION("File path is materialized into the row")
    {
      auto provider = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

      auto const rowPtr = provider.trackRow(helperId);
      REQUIRE(rowPtr);

      auto const expected = Glib::ustring{(runtime.musicRoot() / "test.flac").string()};
      REQUIRE(rowPtr->stringField(rt::TrackField::FilePath) != nullptr);
      CHECK(*rowPtr->stringField(rt::TrackField::FilePath) == expected);
      CHECK(rowPtr->fieldText(rt::TrackField::FilePath) == expected);
    }

    SECTION("Non-existent track")
    {
      auto provider = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
      auto const rowPtr = provider.trackRow(TrackId{999});
      CHECK_FALSE(rowPtr);
    }
  }

  TEST_CASE("TrackRowCache - a library mutation invalidates the cached row it changed", "[gtk][unit][track][row-cache]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
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
    CHECK(rowAfterPtr != rowBeforePtr);
    CHECK(rowAfterPtr->fieldText(rt::TrackField::Title) == "After Import");
    CHECK(rowBeforePtr->fieldText(rt::TrackField::Title) == "Before Import");
  }

  TEST_CASE("TrackRowCache - retained rows preserve bind text across repeated lookup", "[gtk][unit][track][row-cache]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    constexpr std::size_t kRowCount = 5000;
    constexpr std::size_t kReScrollPasses = 20;
    auto ids = std::vector<TrackId>{};
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& library) { ids = seedLibrary(library, kRowCount); }};
    REQUIRE(ids.size() == kRowCount);
    auto cache = TrackRowCache{fixture.runtime().library(), ao::test::englishMessageCatalog()};

    struct RetainedRow final
    {
      Glib::RefPtr<TrackRowObject> rowPtr;
      std::array<Glib::ustring const*, kBindFields.size()> texts{};
      std::array<Glib::ustring, kBindFields.size()> values{};
    };
    auto retained = std::vector<RetainedRow>{};
    retained.reserve(ids.size());
    std::size_t missingRows = 0;
    std::size_t wrongIds = 0;
    std::size_t missingTexts = 0;
    std::size_t coldCharacters = 0;

    // Keep both row ownership and independent text copies across the complete cold walk.
    for (auto const id : ids)
    {
      auto row = RetainedRow{.rowPtr = cache.trackRow(id)};

      if (!row.rowPtr)
      {
        ++missingRows;
        continue;
      }

      wrongIds += static_cast<std::size_t>(row.rowPtr->trackId() != id);

      for (std::size_t column = 0; column < kBindFields.size(); ++column)
      {
        row.texts[column] = row.rowPtr->displayText(kBindFields[column]);

        if (row.texts[column] == nullptr)
        {
          ++missingTexts;
          continue;
        }

        row.values[column] = *row.texts[column];
        coldCharacters += row.texts[column]->size();
      }

      retained.push_back(std::move(row));
    }

    REQUIRE(missingRows == 0);
    REQUIRE(wrongIds == 0);
    REQUIRE(missingTexts == 0);
    REQUIRE(retained.size() == ids.size());
    CHECK(coldCharacters > 0);
    CHECK(cache.cachedRowCount() == kRowCount);

    std::size_t changedRows = 0;
    std::size_t changedTextPointers = 0;
    std::size_t changedTextValues = 0;
    std::size_t warmCharacters = 0;

    for (std::size_t pass = 0; pass < kReScrollPasses; ++pass)
    {
      for (std::size_t index = 0; index < ids.size(); ++index)
      {
        auto const rowPtr = cache.trackRow(ids[index]);

        if (!rowPtr)
        {
          ++missingRows;
          continue;
        }

        wrongIds += static_cast<std::size_t>(rowPtr->trackId() != ids[index]);
        changedRows += static_cast<std::size_t>(rowPtr != retained[index].rowPtr);

        for (std::size_t column = 0; column < kBindFields.size(); ++column)
        {
          auto const* text = rowPtr->displayText(kBindFields[column]);

          if (text == nullptr)
          {
            ++missingTexts;
            continue;
          }

          changedTextPointers += static_cast<std::size_t>(text != retained[index].texts[column]);
          changedTextValues += static_cast<std::size_t>(*text != retained[index].values[column]);
          warmCharacters += text->size();
        }
      }
    }

    CHECK(missingRows == 0);
    CHECK(wrongIds == 0);
    CHECK(missingTexts == 0);
    CHECK(changedRows == 0);
    CHECK(changedTextPointers == 0);
    CHECK(changedTextValues == 0);
    CHECK(warmCharacters == coldCharacters * kReScrollPasses);
    CHECK(cache.cachedRowCount() == kRowCount);
  }
} // namespace ao::gtk::test
