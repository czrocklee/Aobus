// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/rt/PlaybackService.h>

#include <cstdint>
#include <functional>
#include <string>

namespace ao::uimodel
{
  enum class VolumeIndicatorKind : std::uint8_t
  {
    Muted,
    Low,
    Medium,
    High,
  };

  struct VolumeViewState final
  {
    bool visible = false;
    float volume = 1.0F;
    bool isHardwareAssisted = false;
    bool muted = false;
    VolumeIndicatorKind indicatorKind = VolumeIndicatorKind::High;
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
    static VolumeIndicatorKind resolveIndicatorKind(float volume, bool muted) noexcept;
    static std::string resolveTooltip(float volume, bool muted, bool isHardwareAssisted);

  private:
    void applyVolumeTarget(float currentVolume, bool muted, float targetVolume);
    void refresh();

    rt::PlaybackService& _playback;
    std::function<void(VolumeViewState const&)> _onRender;

    async::Subscription _outputDeviceSub;
    async::Subscription _startedSub;
    async::Subscription _volumeSub;
    async::Subscription _mutedSub;
  };
} // namespace ao::uimodel
