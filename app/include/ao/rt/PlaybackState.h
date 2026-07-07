// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include <ao/CoreIds.h>
#include <ao/audio/Backend.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/Transport.h>

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

  inline bool supportsOutputProfile(OutputDeviceSnapshot const& device, audio::ProfileId const& profile)
  {
    return profile != audio::kProfileExclusive || !device.id.empty();
  }

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

  struct NowPlayingInfo final
  {
    TrackId trackId = kInvalidTrackId;
    ListId sourceListId = kInvalidListId;
    ViewId sourceViewId = kInvalidViewId;
    ResourceId coverArtId = kInvalidResourceId;
    std::string title{};
    std::string artist{};
    std::string album{};

    bool operator==(NowPlayingInfo const&) const = default;
  };

  struct PlaybackModeState final
  {
    ShuffleMode shuffle = ShuffleMode::Off;
    RepeatMode repeat = RepeatMode::Off;

    bool operator==(PlaybackModeState const&) const = default;
  };

  struct VolumeState final
  {
    float level = 1.0F;
    bool muted = false;
    bool available = false;
    bool hardwareAssisted = false;

    bool operator==(VolumeState const&) const = default;
  };

  struct OutputState final
  {
    OutputDeviceSelection selectedDevice{};
    std::vector<OutputBackendSnapshot> availableBackends{};

    bool operator==(OutputState const&) const = default;
  };

  struct QualityState final
  {
    audio::Quality sourceQuality = audio::Quality::Unknown;
    audio::Quality pipelineQuality = audio::Quality::Unknown;
    audio::Quality overall = audio::Quality::Unknown;
    bool fullyVerified = true;
    std::vector<audio::NodeQualityAssessment> assessments{};

    bool operator==(QualityState const&) const = default;
  };

  struct PlaybackState final
  {
    audio::Transport transport = audio::Transport::Idle;
    std::chrono::milliseconds elapsed{0};
    std::chrono::milliseconds duration{0};
    bool ready = false;

    NowPlayingInfo nowPlaying{};
    PlaybackModeState mode{};
    VolumeState volume{};
    OutputState output{};
    QualityState quality{};
    std::uint64_t revision = 0;
  };
} // namespace ao::rt
