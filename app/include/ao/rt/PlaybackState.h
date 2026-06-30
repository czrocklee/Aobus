// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include <ao/CoreIds.h>
#include <ao/audio/Backend.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ao::rt
{
  struct OutputProfileSnapshot final
  {
    audio::ProfileId id{};
    std::string name{};
    std::string description{};

    bool operator==(OutputProfileSnapshot const&) const = default;
  };

  struct OutputDeviceSnapshot final
  {
    audio::DeviceId id{};
    std::string displayName{};
    std::string description{};
    bool isDefault = false;
    audio::BackendId backendId{};
    audio::DeviceCapabilities capabilities{};

    bool operator==(OutputDeviceSnapshot const&) const = default;
  };

  struct OutputBackendSnapshot final
  {
    audio::BackendId id{};
    std::string name{};
    std::string description{};
    std::string iconName{};
    std::vector<OutputProfileSnapshot> supportedProfiles{};
    std::vector<OutputDeviceSnapshot> devices{};

    bool operator==(OutputBackendSnapshot const&) const = default;
  };

  struct OutputDeviceSelection final
  {
    audio::BackendId backendId{};
    audio::DeviceId deviceId{};
    audio::ProfileId profileId{};

    bool operator==(OutputDeviceSelection const&) const = default;
  };

  enum class ShuffleMode : std::uint8_t
  {
    Off,
    On,
  };

  enum class RepeatMode : std::uint8_t
  {
    Off,
    One,
    All,
  };

  struct PlaybackState final
  {
    audio::Transport transport = audio::Transport::Idle;
    TrackId trackId{};
    ListId sourceListId = kInvalidListId;
    ViewId sourceViewId = kInvalidViewId;
    std::string trackTitle{};
    std::string trackArtist{};
    std::chrono::milliseconds elapsed{0};
    std::chrono::milliseconds duration{0};
    float volume = 1.0F;
    bool muted = false;
    bool volumeAvailable = false;
    bool volumeIsHardwareAssisted = false;
    bool ready = false;

    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode repeatMode = RepeatMode::Off;

    OutputDeviceSelection selectedOutputDevice{};
    std::vector<OutputBackendSnapshot> availableOutputBackends{};
    audio::flow::Graph flow{};
    audio::Quality quality = audio::Quality::Unknown;
    std::vector<audio::NodeQualityAssessment> qualityAssessments{};
    std::uint64_t revision = 0;
  };
} // namespace ao::rt
