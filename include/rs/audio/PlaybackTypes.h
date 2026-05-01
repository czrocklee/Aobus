// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/Type.h>
#include <rs/audio/AudioFormat.h>
#include <rs/audio/BackendTypes.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rs::audio
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

  struct TrackPlaybackDescriptor final
  {
    rs::TrackId trackId{};
    std::filesystem::path filePath;
    std::string title;
    std::string artist;
    std::string album;
    std::optional<rs::ResourceId> coverArtId;
    std::uint32_t durationMs = 0;
    std::uint32_t sampleRateHint = 0;
    std::uint8_t channelsHint = 0;
    std::uint8_t bitDepthHint = 0;
  };

  /**
   * @brief Represents the current state of a backend and its devices.
   */
  struct BackendSnapshot final
  {
    rs::audio::BackendKind kind = rs::audio::BackendKind::None;
    std::string displayName;
    std::string shortName;
    std::string id;
    std::vector<rs::audio::AudioDevice> devices;

    bool operator==(BackendSnapshot const&) const = default;
  };

  struct BackendRouteAnchor final
  {
    rs::audio::BackendKind backend = rs::audio::BackendKind::None;
    std::string id;

    bool operator==(BackendRouteAnchor const&) const = default;
  };

  struct EngineRouteSnapshot final
  {
    rs::audio::AudioGraph graph;
    std::optional<BackendRouteAnchor> anchor;

    bool operator==(EngineRouteSnapshot const&) const = default;
  };

  struct PlaybackSnapshot final
  {
    TransportState state = TransportState::Idle;
    rs::audio::BackendKind backend = rs::audio::BackendKind::None;
    std::string trackTitle;
    std::string trackArtist;
    std::uint32_t positionMs = 0;
    std::uint32_t durationMs = 0;
    std::uint32_t bufferedMs = 0;
    std::uint32_t underrunCount = 0;
    std::string statusText;

    // Device info
    std::string currentDeviceId;
    std::vector<BackendSnapshot> availableBackends;

    // Semantic graph data
    rs::audio::AudioGraph graph;
    rs::audio::AudioQuality quality = rs::audio::AudioQuality::Unknown;
    std::string qualityTooltip;
  };
} // namespace rs::audio
