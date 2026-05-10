// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <test/unit/lmdb/TestUtils.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::app::test
{
  struct TrackSpec final
  {
    std::string title = "Track";
    std::string artist = "Artist";
    std::string album = "Album";
    std::string albumArtist{};
    std::string genre{};
    std::string composer{};
    std::string work{};
    std::uint16_t year = 2020;
    std::uint16_t discNumber = 1;
    std::uint16_t trackNumber = 1;
    std::uint32_t durationMs = 200000;
  };

  inline TrackSpec makeSpec(std::string_view title, std::uint16_t year)
  {
    return TrackSpec{.title = std::string{title}, .year = year};
  }

  class TestMusicLibrary final
  {
  public:
    TestMusicLibrary()
      : _tempDir{}, _library{_tempDir.path()}
    {
    }

    ao::library::MusicLibrary& library() { return _library; }
    ao::library::MusicLibrary const& library() const { return _library; }

    ao::TrackId addTrack(TrackSpec const& spec)
    {
      auto txn = _library.writeTransaction();
      auto writer = _library.tracks().writer(txn);
      auto builder = ao::library::TrackBuilder::createNew();
      builder.metadata()
        .title(spec.title)
        .artist(spec.artist)
        .album(spec.album)
        .albumArtist(spec.albumArtist)
        .genre(spec.genre)
        .composer(spec.composer)
        .work(spec.work)
        .year(spec.year)
        .discNumber(spec.discNumber)
        .trackNumber(spec.trackNumber);
      builder.property()
        .uri("/tmp/test.flac")
        .durationMs(spec.durationMs)
        .bitrate(320000)
        .sampleRate(44100)
        .channels(2)
        .bitDepth(16);
      auto hotData = builder.serializeHot(txn, _library.dictionary());
      auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
      auto [id, _] = writer.createHotCold(hotData, coldData);
      txn.commit();
      return id;
    }

    ao::TrackId addTrack(std::string_view title) { return addTrack(TrackSpec{.title = std::string{title}}); }

  private:
    TempDir _tempDir;
    ao::library::MusicLibrary _library;
  };
}
