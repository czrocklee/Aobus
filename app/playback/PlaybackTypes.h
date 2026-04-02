// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/Type.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace app::playback
{

  // Common audio sample type for internal PCM representation
  using AudioSample = float;

  enum class TransportState
  {
    Idle,
    Opening,
    Buffering,
    Playing,
    Paused,
    Seeking,
    Stopping,
    Error,
  };

  enum class BackendKind
  {
    None,
    PipeWire,
    AlsaExclusive,
  };

  struct StreamFormat final
  {
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    std::uint8_t bitDepth = 0;
    bool isFloat = false;
    bool isInterleaved = true;
  };

  struct TrackPlaybackDescriptor final
  {
    rs::core::TrackId trackId{};
    std::filesystem::path filePath;
    std::string title;
    std::string artist;
    std::string album;
    std::optional<rs::core::ResourceId> coverArtId;
    std::uint32_t durationMs = 0;
    std::uint32_t sampleRateHint = 0;
    std::uint8_t channelsHint = 0;
    std::uint8_t bitDepthHint = 0;
  };

  struct PlaybackSnapshot final
  {
    TransportState state = TransportState::Idle;
    BackendKind backend = BackendKind::None;
    std::string trackTitle;
    std::string trackArtist;
    std::string statusText;
    std::uint32_t durationMs = 0;
    std::uint32_t positionMs = 0;
    std::uint32_t bufferedMs = 0;
    std::uint32_t underrunCount = 0;
    std::optional<StreamFormat> activeFormat;
  };

} // namespace app::playback
