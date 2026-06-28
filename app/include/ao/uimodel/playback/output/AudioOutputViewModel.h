// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ao::uimodel
{
  struct AudioOutputRow final
  {
    enum class Kind : std::uint8_t
    {
      BackendHeader,
      DeviceProfile,
    };

    Kind kind = Kind::BackendHeader;
    audio::BackendId backendId;
    audio::DeviceId deviceId;
    audio::ProfileId profileId;
    std::string title;
    std::string description;
    bool isActive = false;
    bool isExclusive = false;
  };

  struct AudioOutputViewState final
  {
    std::vector<AudioOutputRow> rows;
    std::string backendSummary;
    std::string outputStatus;
    bool hasActiveOutput = false;
  };

  class AudioOutputViewModel final
  {
  public:
    AudioOutputViewModel(rt::PlaybackService& playback, std::function<void(AudioOutputViewState const&)> onRender);

    AudioOutputViewModel(AudioOutputViewModel const&) = delete;
    AudioOutputViewModel& operator=(AudioOutputViewModel const&) = delete;
    AudioOutputViewModel(AudioOutputViewModel&&) = delete;
    AudioOutputViewModel& operator=(AudioOutputViewModel&&) = delete;

    ~AudioOutputViewModel() = default;

    void selectOutput(audio::BackendId const& backendId,
                      audio::DeviceId const& deviceId,
                      audio::ProfileId const& profileId);

    void refresh();

  private:
    rt::PlaybackService& _playback;
    std::function<void(AudioOutputViewState const&)> _onRender;

    rt::Subscription _outputChangedSub;
  };
} // namespace ao::uimodel
