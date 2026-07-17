// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/PlaybackService.h>
#include <ao/rt/Subscription.h>

#include <functional>
#include <string>

namespace ao::uimodel
{
  struct VolumeViewState final
  {
    bool visible = false;
    float volume = 1.0F;
    bool isHardwareAssisted = false;
    bool muted = false;
    std::string iconName{};
    std::string tooltip{};
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
    void handleMutedChanged(bool muted);
    void toggleMuted();
    void handleScroll(double scrollDy);
    void adjustVolume(float delta);

    static float resolveVolumeOffset(double widgetWidth, double offsetX, float currentDragStartVolume = 0.0F);
    static float resolveVolumeScroll(float currentVolume, double scrollDy);
    static std::string resolveIconName(float volume, bool muted);
    static std::string resolveTooltip(float volume, bool muted, bool isHardwareAssisted);

  private:
    void applyVolumeTarget(float currentVolume, bool muted, float targetVolume);
    void refresh();

    rt::PlaybackService& _playback;
    std::function<void(VolumeViewState const&)> _onRender;

    rt::Subscription _outputDeviceSub;
    rt::Subscription _startedSub;
    rt::Subscription _volumeSub;
    rt::Subscription _mutedSub;
  };
} // namespace ao::uimodel
