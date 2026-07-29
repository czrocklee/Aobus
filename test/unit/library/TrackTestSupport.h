// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
  class TrackBuilder;
  class TrackView;
  class WriteTransaction;
}

namespace ao::library::test
{
  struct TrackSpec final
  {
    std::string title = "Track";
    std::string artist = "Artist";
    std::string album = "Album";
    std::string albumArtist{};
    std::string genre{};
    std::string composer{};
    std::string conductor{};
    std::string ensemble{};
    std::string work{};
    std::string movement{};
    std::string soloist{};
    std::string uri = "test.flac";
    std::vector<std::string> tags{};
    std::vector<std::pair<std::string, std::string>> customMetadata{};
    ResourceId coverArtId = kInvalidResourceId;
    std::uint16_t year = 2020;
    std::uint16_t discNumber = 1;
    std::uint16_t discTotal = 0;
    std::uint16_t trackNumber = 1;
    std::uint16_t trackTotal = 0;
    std::uint16_t movementNumber = 0;
    std::uint16_t movementTotal = 0;
    std::chrono::milliseconds duration = std::chrono::seconds{200};
    Bitrate bitrate = Bitrate{320000};
    SampleRate sampleRate = SampleRate{44100};
    Channels channels = Channels{2};
    BitDepth bitDepth = BitDepth{16};
    AudioCodec codec = AudioCodec::Unknown;
  };

  TrackSpec makeTrackSpec(std::string_view title, std::uint16_t year = 2020);
  TrackSpec makeEmptyTrackSpec(std::string_view uri);
  void applyTrackSpec(TrackBuilder& builder, TrackSpec const& spec);
  TrackSpec trackSpecFromView(MusicLibrary const& library, TrackView const& view);
  TrackId addTrack(MusicLibrary& library, WriteTransaction& transaction, TrackSpec const& spec);
  TrackId addTrack(MusicLibrary& library, TrackSpec const& spec);
  void mutateTrack(MusicLibrary& library, TrackId id, std::move_only_function<void(TrackBuilder&)> mutate);
  void updateTrackSpec(MusicLibrary& library, TrackId id, std::move_only_function<void(TrackSpec&)> updater);
} // namespace ao::library::test
