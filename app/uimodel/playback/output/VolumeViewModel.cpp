// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <utility>

namespace ao::uimodel
{
  VolumeViewModel::VolumeViewModel(rt::PlaybackService& playback, std::function<void(VolumeViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    auto const refreshCallback = [this] { refresh(); };
    _outputDeviceSub = _playback.onOutputDeviceChanged([refreshCallback](auto const&) { refreshCallback(); });
    _startedSub = _playback.onStarted(refreshCallback);
    _volumeSub = _playback.onVolumeChanged([refreshCallback](float) { refreshCallback(); });
    _mutedSub = _playback.onMutedChanged([refreshCallback](bool) { refreshCallback(); });
    refresh();
  }

  void VolumeViewModel::handleVolumeChanged(float volume)
  {
    _playback.setVolume(volume);
  }

  void VolumeViewModel::handleMutedChanged(bool muted)
  {
    _playback.setMuted(muted);
  }

  void VolumeViewModel::toggleMuted()
  {
    _playback.setMuted(!_playback.state().volume.muted);
  }

  void VolumeViewModel::handleScroll(double scrollDy)
  {
    auto const volume = _playback.state().volume;
    applyVolumeTarget(volume.level, volume.muted, resolveVolumeScroll(volume.level, scrollDy));
  }

  void VolumeViewModel::adjustVolume(float const delta)
  {
    auto const volume = _playback.state().volume;
    applyVolumeTarget(volume.level, volume.muted, volume.level + delta);
  }

  void VolumeViewModel::applyVolumeTarget(float const currentVolume, bool const muted, float const targetVolume)
  {
    auto const newVolume = std::clamp(targetVolume, 0.0F, 1.0F);

    if (muted && newVolume > currentVolume)
    {
      _playback.setMuted(false);
    }

    _playback.setVolume(newVolume);
  }

  void VolumeViewModel::refresh()
  {
    auto const& state = _playback.state();

    auto view = VolumeViewState{
      .visible = state.volume.available,
      .volume = state.volume.level,
      .isHardwareAssisted = state.volume.hardwareAssisted,
      .muted = state.volume.muted,
      .indicatorKind = resolveIndicatorKind(state.volume.level, state.volume.muted),
      .tooltip = resolveTooltip(state.volume.level, state.volume.muted, state.volume.hardwareAssisted),
    };

    if (_onRender)
    {
      _onRender(view);
    }
  }

  float VolumeViewModel::resolveVolumeOffset(double widgetWidth, double offsetX, float currentDragStartVolume)
  {
    if (widgetWidth <= 0.0)
    {
      return currentDragStartVolume;
    }

    float const delta = static_cast<float>(offsetX / widgetWidth);
    return std::clamp(currentDragStartVolume + delta, 0.0F, 1.0F);
  }

  float VolumeViewModel::resolveVolumeScroll(float currentVolume, double scrollDy)
  {
    constexpr float kScrollStep = 0.02F;
    float const delta = (scrollDy > 0) ? -kScrollStep : kScrollStep;
    return std::clamp(currentVolume + delta, 0.0F, 1.0F);
  }

  VolumeIndicatorKind VolumeViewModel::resolveIndicatorKind(float const volume, bool const muted) noexcept
  {
    if (muted || volume <= 0.0F)
    {
      return VolumeIndicatorKind::Muted;
    }

    constexpr float kLowVolumeThreshold = 0.33F;
    constexpr float kMediumVolumeThreshold = 0.66F;

    if (volume <= kLowVolumeThreshold)
    {
      return VolumeIndicatorKind::Low;
    }

    if (volume <= kMediumVolumeThreshold)
    {
      return VolumeIndicatorKind::Medium;
    }

    return VolumeIndicatorKind::High;
  }

  std::string VolumeViewModel::resolveTooltip(float volume, bool muted, bool isHardwareAssisted)
  {
    std::int32_t const percent = static_cast<std::int32_t>(std::round(volume * 100.0F));

    std::string text = std::format("Volume: {}%", percent);

    if (muted)
    {
      text += " (Muted)";
    }
    else if (isHardwareAssisted)
    {
      text += " (Hardware)";
    }

    return text;
  }
} // namespace ao::uimodel
