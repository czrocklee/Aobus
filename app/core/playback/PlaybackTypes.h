// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "../AudioFormat.h"
#include "core/backend/BackendTypes.h"
#include <rs/core/Type.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace app::core::playback
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

  /**
   * @brief Represents the current state of a backend and its devices.
   */
  struct BackendSnapshot final
  {
    backend::BackendKind kind = backend::BackendKind::None;
    std::string displayName;
    std::string shortName;
    std::string id;
    std::vector<backend::AudioDevice> devices;

    bool operator==(BackendSnapshot const&) const = default;
  };

  struct BackendRouteAnchor final
  {
    backend::BackendKind backend = backend::BackendKind::None;
    std::string id;

    bool operator==(BackendRouteAnchor const&) const = default;
  };

  struct EngineRouteSnapshot final
  {
    backend::AudioGraph graph;
    std::optional<BackendRouteAnchor> anchor;

    bool operator==(EngineRouteSnapshot const&) const = default;
  };

  struct PlaybackSnapshot final
  {
    TransportState state = TransportState::Idle;
    backend::BackendKind backend = backend::BackendKind::None;
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
    backend::AudioGraph graph;
    backend::AudioQuality quality = backend::AudioQuality::Unknown;
    std::string qualityTooltip;
  };

} // namespace app::core::playback
