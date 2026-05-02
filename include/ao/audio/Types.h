// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/Type.h>
#include <ao/audio/Format.h>
#include <ao/audio/Backend.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ao::audio
{
  // Common audio sample type for internal PCM representation
  using Sample = float;

  enum class Transport
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
    ao::TrackId trackId{};
    std::filesystem::path filePath;
    std::string title;
    std::string artist;
    std::string album;
    std::optional<ao::ResourceId> coverArtId;
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
    BackendKind kind = BackendKind::None;
    std::string displayName;
    std::string shortName;
    std::string id;
    std::vector<Device> devices;

    bool operator==(BackendSnapshot const&) const = default;
  };

  struct BackendRouteAnchor final
  {
    BackendKind backend = BackendKind::None;
    std::string id;

    bool operator==(BackendRouteAnchor const&) const = default;
  };

  struct EngineRouteSnapshot final
  {
    flow::Graph flow;
    std::optional<BackendRouteAnchor> anchor;

    bool operator==(EngineRouteSnapshot const&) const = default;
  };

  struct Snapshot final
  {
    Transport transport = Transport::Idle;
    BackendKind backend = BackendKind::None;
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
    flow::Graph flow;
    Quality quality = Quality::Unknown;
    std::string qualityTooltip;
  };
} // namespace ao::audio
