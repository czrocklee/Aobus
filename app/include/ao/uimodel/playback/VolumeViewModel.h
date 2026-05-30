// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>

#include <functional>

namespace ao::uimodel::playback
{
  struct VolumeViewState final
  {
    bool visible = false;
    float volume = 1.0F;
    bool isHardwareAssisted = false;
  };

  class VolumeViewModel final
  {
  public:
    VolumeViewModel(rt::PlaybackService& playback, std::function<void(VolumeViewState const&)> onRender);

    VolumeViewModel(VolumeViewModel const&) = delete;
    VolumeViewModel& operator=(VolumeViewModel const&) = delete;
    VolumeViewModel(VolumeViewModel&&) = delete;
    VolumeViewModel& operator=(VolumeViewModel&&) = delete;

    ~VolumeViewModel() = default;

    void handleVolumeChanged(float volume);

    static float resolveVolumeOffset(double widgetWidth, double offsetX, float currentDragStartVolume = 0.0F);
    static float resolveVolumeScroll(float currentVolume, double scrollDy);

  private:
    void refresh();

    rt::PlaybackService& _playback;
    std::function<void(VolumeViewState const&)> _onRender;

    rt::Subscription _outputSub;
    rt::Subscription _startedSub;
    rt::Subscription _volumeSub;
  };
} // namespace ao::uimodel::playback
