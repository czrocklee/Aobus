// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackRowCache.h"

#include "test/unit/lmdb/TestUtils.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <ao/audio/Types.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/refptr.h>
#include <gtkmm/application.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace ao::gtk::test
{
  namespace
  {
    using namespace ao::lmdb::test;

    struct TrackSpec final
    {
      std::string title = "Title";
      std::string artist = "Artist";
      std::string album = "Album";
      std::string albumArtist = "AlbumArtist";
      std::string genre = "Genre";
      std::uint16_t year = 2020;
      std::uint16_t trackNumber = 1;
      std::uint16_t discNumber = 1;
      std::uint32_t durationMs = 180000;
    };

    class TestMusicLibrary final
    {
    public:
      TestMusicLibrary()
        : _tempDir{}, _library{_tempDir.path(), _tempDir.path()}
      {
      }

      library::MusicLibrary& library() { return _library; }

      TrackId addTrack(TrackSpec const& spec)
      {
        auto txn = _library.writeTransaction();
        auto writer = _library.tracks().writer(txn);

        auto builder = library::TrackBuilder::createNew();
        builder.metadata()
          .title(spec.title)
          .artist(spec.artist)
          .album(spec.album)
          .albumArtist(spec.albumArtist)
          .genre(spec.genre)
          .year(spec.year)
          .trackNumber(spec.trackNumber)
          .discNumber(spec.discNumber);
        builder.property()
          .uri("/tmp/test.flac")
          .durationMs(spec.durationMs)
          .bitrate(320000)
          .sampleRate(44100)
          .channels(2)
          .bitDepth(16);

        auto const [hotData, coldData] = builder.serialize(txn, _library.dictionary(), _library.resources());
        auto [id, _] = writer.createHotCold(hotData, coldData);
        txn.commit();
        return id;
      }

    private:
      TempDir _tempDir;
      library::MusicLibrary _library;
    };
  } // namespace

  TEST_CASE("TrackRowCache loads track data correctly", "[app][unit][model]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.row_cache_test");
    auto testLibrary = TestMusicLibrary{};

    SECTION("Basic data loading")
    {
      auto spec1 = TrackSpec{};
      spec1.artist = "Artist 1";
      spec1.album = "Album 1";
      spec1.title = "Track 1";
      spec1.genre = "Genre 1";
      spec1.year = 2021;
      spec1.trackNumber = 1;
      spec1.durationMs = 180000;

      auto spec2 = TrackSpec{};
      spec2.title = "Track 2";
      spec2.durationMs = 240000;

      auto const id1 = testLibrary.addTrack(spec1);
      auto const id2 = testLibrary.addTrack(spec2);

      auto provider = TrackRowCache{testLibrary.library()};

      auto const row1 = provider.trackRow(id1);
      REQUIRE(row1);
      CHECK(row1->fieldText(rt::TrackField::Artist) == "Artist 1");
      CHECK(row1->fieldText(rt::TrackField::Album) == "Album 1");
      CHECK(row1->fieldText(rt::TrackField::Title) == "Track 1");
      CHECK(row1->fieldText(rt::TrackField::Genre) == "Genre 1");
      CHECK(row1->year() == 2021);
      CHECK(row1->trackNumber() == 1);
      CHECK(row1->duration().count() == 180000);

      auto const row2 = provider.trackRow(id2);
      REQUIRE(row2);
      CHECK(row2->fieldText(rt::TrackField::Title) == "Track 2");
      CHECK(row2->duration().count() == 240000);

      // Verify playing properties and setters/getters
      CHECK_FALSE(row1->isPlaying());
      row1->setPlaying(true);
      CHECK(row1->isPlaying());
      auto const proxy = row1->property_playing();
      CHECK(proxy.get_value() == true);

      // Verify custom string fields and failure paths
      CHECK(row1->setStringField(rt::TrackField::Artist, "New Artist"));
      CHECK(row1->fieldText(rt::TrackField::Artist) == "New Artist");
      CHECK_FALSE(row1->setStringField(rt::TrackField::Duration, "Failed"));

      // Verify other metadata and resource/playback properties
      row1->setYear(2025);
      row1->setDiscNumber(2);
      row1->setTotalDiscs(3);
      row1->setTrackNumber(4);
      row1->setTotalTracks(10);
      CHECK(row1->year() == 2025);
      CHECK(row1->discNumber() == 2);
      CHECK(row1->totalDiscs() == 3);
      CHECK(row1->trackNumber() == 4);
      CHECK(row1->totalTracks() == 10);
      CHECK(row1->sampleRate() == 44100);
      CHECK(row1->channels() == 2);
      CHECK(row1->bitDepth() == 16);
    }

    SECTION("Cache helper methods")
    {
      auto spec = TrackSpec{};
      spec.durationMs = 120000;
      auto const id = testLibrary.addTrack(spec);

      auto provider = TrackRowCache{testLibrary.library()};

      auto const optUri = provider.uriPath(id);
      REQUIRE(optUri.has_value());
      CHECK(optUri->string() == "/tmp/test.flac");

      auto const optDesc = provider.playbackDescriptor(id);
      REQUIRE(optDesc.has_value());
      CHECK(optDesc->durationMs == 120000);

      auto const optCover = provider.coverArtId(id);
      CHECK_FALSE(optCover.has_value());

      provider.clearCache();
      provider.remove(id);
    }

    SECTION("Caching works")
    {
      auto const id1 = testLibrary.addTrack({});
      auto provider = TrackRowCache{testLibrary.library()};

      auto const row1A = provider.trackRow(id1);
      auto const row1B = provider.trackRow(id1);

      REQUIRE(row1A);
      REQUIRE(row1B);
      CHECK(row1A == row1B);
    }

    SECTION("Invalidation")
    {
      auto const id1 = testLibrary.addTrack({});
      auto provider = TrackRowCache{testLibrary.library()};

      auto const row1 = provider.trackRow(id1);
      REQUIRE(row1);
      provider.invalidate(id1);

      auto const row1New = provider.trackRow(id1);
      CHECK(row1 != row1New);
    }

    SECTION("Dictionary resolution")
    {
      testLibrary.addTrack({.title = "Test Dictionary String"});
      auto provider = TrackRowCache{testLibrary.library()};
      auto const id = DictionaryId{1}; // Assuming ID 1 exists because it's the first string added to the dict

      auto const& name = provider.resolveDictionaryString(id);
      CHECK_FALSE(name.empty());

      provider.clearCache();
    }

    SECTION("Non-existent track")
    {
      auto provider = TrackRowCache{testLibrary.library()};
      auto const row = provider.trackRow(TrackId{999});
      CHECK_FALSE(row);
    }
  }
} // namespace ao::gtk::test
