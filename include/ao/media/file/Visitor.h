// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/PictureType.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ao::media::file
{
  enum class TextField : std::uint8_t
  {
    Title,
    Artist,
    Album,
    AlbumArtist,
    Composer,
    Conductor,
    Ensemble,
    Genre,
    Work,
    Movement,
    Soloist,
  };

  enum class NumberField : std::uint8_t
  {
    Year,
    TrackNumber,
    TrackTotal,
    DiscNumber,
    DiscTotal,
    MovementNumber,
    MovementTotal,
  };

  class Visitor
  {
  public:
    Visitor() = default;
    virtual ~Visitor() = default;

    Visitor(Visitor const&) = delete;
    Visitor& operator=(Visitor const&) = delete;
    Visitor(Visitor&&) = delete;
    Visitor& operator=(Visitor&&) = delete;

    virtual void text(TextField field, std::string_view value) = 0;
    virtual void number(NumberField field, std::uint16_t value) = 0;
    virtual void codec(AudioCodec value) = 0;
    virtual void duration(std::chrono::milliseconds duration) = 0;
    virtual void bitrate(Bitrate value) = 0;
    virtual void sampleRate(SampleRate value) = 0;
    virtual void channels(Channels value) = 0;
    virtual void bitDepth(BitDepth value) = 0;
    virtual void picture(PictureType type, std::span<std::byte const> bytes) = 0;
  };
} // namespace ao::media::file
