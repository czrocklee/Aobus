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
  struct OutputDeviceRow final
  {
    enum class Kind : std::uint8_t
    {
      BackendHeader,
      DeviceProfile,
    };

    Kind kind = Kind::BackendHeader;
    audio::BackendId backendId{};
    audio::DeviceId deviceId{};
    audio::ProfileId profileId{};
    std::string title{};
    std::string description{};
    bool isActive = false;
    bool isExclusive = false;
  };

  struct OutputDeviceViewState final
  {
    std::vector<OutputDeviceRow> rows{};
    std::string outputBackendSummary{};
    std::string outputDeviceStatus{};
    bool hasActiveOutputDevice = false;
  };

  class OutputDeviceViewModel final
  {
  public:
    OutputDeviceViewModel(rt::PlaybackService& playback, std::function<void(OutputDeviceViewState const&)> onRender);

    OutputDeviceViewModel(OutputDeviceViewModel const&) = delete;
    OutputDeviceViewModel& operator=(OutputDeviceViewModel const&) = delete;
    OutputDeviceViewModel(OutputDeviceViewModel&&) = delete;
    OutputDeviceViewModel& operator=(OutputDeviceViewModel&&) = delete;

    ~OutputDeviceViewModel() = default;

    void selectOutputDevice(audio::BackendId const& backendId,
                            audio::DeviceId const& deviceId,
                            audio::ProfileId const& profileId);

    void refresh();

  private:
    rt::PlaybackService& _playback;
    std::function<void(OutputDeviceViewState const&)> _onRender;

    rt::Subscription _outputDeviceChangedSub;
  };
} // namespace ao::uimodel
