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
      std::chrono::milliseconds duration = std::chrono::minutes{3};
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
          .duration(spec.duration)
          .bitrate(Bitrate{320000})
          .sampleRate(SampleRate{44100})
          .channels(Channels{2})
          .bitDepth(BitDepth{16});

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
    auto const appPtr = Gtk::Application::create("io.github.aobus.row_cache_test");
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
      spec1.duration = std::chrono::minutes{3};

      auto spec2 = TrackSpec{};
      spec2.title = "Track 2";
      spec2.duration = std::chrono::minutes{4};

      auto const id1 = testLibrary.addTrack(spec1);
      auto const id2 = testLibrary.addTrack(spec2);

      auto provider = TrackRowCache{testLibrary.library()};

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

      // Verify playing properties and setters/getters
      CHECK_FALSE(row1Ptr->isPlaying());
      row1Ptr->setPlaying(true);
      CHECK(row1Ptr->isPlaying());
      auto const proxy = row1Ptr->property_playing();
      CHECK(proxy.get_value() == true);

      // Verify custom string fields and failure paths
      CHECK(row1Ptr->setStringField(rt::TrackField::Artist, "New Artist"));
      CHECK(row1Ptr->fieldText(rt::TrackField::Artist) == "New Artist");
      CHECK_FALSE(row1Ptr->setStringField(rt::TrackField::Duration, "Failed"));

      // Verify other metadata and resource/playback properties
      row1Ptr->setYear(2025);
      row1Ptr->setDiscNumber(2);
      row1Ptr->setTotalDiscs(3);
      row1Ptr->setTrackNumber(4);
      row1Ptr->setTotalTracks(10);
      CHECK(row1Ptr->year() == 2025);
      CHECK(row1Ptr->discNumber() == 2);
      CHECK(row1Ptr->totalDiscs() == 3);
      CHECK(row1Ptr->trackNumber() == 4);
      CHECK(row1Ptr->totalTracks() == 10);
      CHECK(row1Ptr->sampleRate() == 44100);
      CHECK(row1Ptr->channels() == 2);
      CHECK(row1Ptr->bitDepth() == 16);
    }

    SECTION("Cache helper methods")
    {
      auto spec = TrackSpec{};
      spec.duration = std::chrono::minutes{2};
      auto const id = testLibrary.addTrack(spec);

      auto provider = TrackRowCache{testLibrary.library()};

      auto const optUri = provider.uriPath(id);
      REQUIRE(optUri.has_value());
      CHECK(optUri->string() == "/tmp/test.flac");

      auto const optDesc = provider.playbackDescriptor(id);
      REQUIRE(optDesc.has_value());
      CHECK(optDesc->duration == std::chrono::minutes{2});

      auto const optCover = provider.coverArtId(id);
      CHECK_FALSE(optCover.has_value());

      provider.clearCache();
      provider.remove(id);
    }

    SECTION("Caching works")
    {
      auto const id1 = testLibrary.addTrack({});
      auto provider = TrackRowCache{testLibrary.library()};

      auto const row1APtr = provider.trackRow(id1);
      auto const row1BPtr = provider.trackRow(id1);

      REQUIRE(row1APtr);
      REQUIRE(row1BPtr);
      CHECK(row1APtr == row1BPtr);
    }

    SECTION("Invalidation")
    {
      auto const id1 = testLibrary.addTrack({});
      auto provider = TrackRowCache{testLibrary.library()};

      auto const row1Ptr = provider.trackRow(id1);
      REQUIRE(row1Ptr);
      provider.invalidate(id1);

      auto const row1NewPtr = provider.trackRow(id1);
      CHECK(row1Ptr != row1NewPtr);
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
      auto const rowPtr = provider.trackRow(TrackId{999});
      CHECK_FALSE(rowPtr);
    }
  }
} // namespace ao::gtk::test
