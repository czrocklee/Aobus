// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/PictureType.h>
#include <ao/library/TrackBuilder.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  // Owning value snapshot of a TrackBuilder. It releases borrowed LMDB, YAML,
  // and media-file views before a write transaction begins.
  class TrackBuilderSnapshot final
  {
  public:
    explicit TrackBuilderSnapshot(library::TrackBuilder const& source);

    library::TrackBuilder makeBuilder() const;

  private:
    struct Cover final
    {
      PictureType type = PictureType::FrontCover;
      std::variant<ResourceId, std::vector<std::byte>> source;
    };

    std::string _title;
    std::string _artist;
    std::string _album;
    std::string _albumArtist;
    std::string _composer;
    std::string _conductor;
    std::string _ensemble;
    std::string _genre;
    std::string _work;
    std::string _movement;
    std::string _soloist;
    std::string _uri;
    std::uint16_t _year = 0;
    std::uint16_t _trackNumber = 0;
    std::uint16_t _trackTotal = 0;
    std::uint16_t _discNumber = 0;
    std::uint16_t _discTotal = 0;
    std::uint16_t _movementNumber = 0;
    std::uint16_t _movementTotal = 0;
    std::chrono::milliseconds _duration{0};
    Bitrate _bitrate{};
    SampleRate _sampleRate{};
    AudioCodec _codec = AudioCodec::Unknown;
    Channels _channels{};
    BitDepth _bitDepth{};
    std::vector<std::string> _tags;
    std::vector<Cover> _covers;
    std::vector<std::pair<std::string, std::string>> _customMetadata;
  };
} // namespace ao::rt
