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
    _playback.setMuted(!_playback.state().muted);
  }

  void VolumeViewModel::handleScroll(double scrollDy)
  {
    auto const& state = _playback.state();
    float const newVolume = resolveVolumeScroll(state.volume, scrollDy);

    // Mute policy on scroll
    if (state.muted && newVolume > state.volume)
    {
      _playback.setMuted(false);
    }
    else if (newVolume <= 0.0F && !state.muted)
    {
      // Wait, should scrolling down to 0 explicitly set mute?
      // The plan says: "If scroll drives volume to 0, set volume to 0; visual state becomes muted. Explicit `muted` can
      // remain false unless the user invoked mute." So no explicit setMuted(true) here, just setVolume(0.0F).
    }

    _playback.setVolume(newVolume);
  }

  void VolumeViewModel::refresh()
  {
    auto const& state = _playback.state();

    auto view = VolumeViewState{};
    view.visible = state.volumeAvailable;
    view.volume = state.volume;
    view.isHardwareAssisted = state.volumeIsHardwareAssisted;
    view.muted = state.muted;
    view.iconName = resolveIconName(state.volume, state.muted);
    view.tooltip = resolveTooltip(state.volume, state.muted, state.volumeIsHardwareAssisted);

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

  std::string VolumeViewModel::resolveIconName(float volume, bool muted)
  {
    if (muted || volume <= 0.0F)
    {
      return "audio-volume-muted-symbolic";
    }

    constexpr float kLowVolumeThreshold = 0.33F;
    constexpr float kMediumVolumeThreshold = 0.66F;

    if (volume <= kLowVolumeThreshold)
    {
      return "audio-volume-low-symbolic";
    }

    if (volume <= kMediumVolumeThreshold)
    {
      return "audio-volume-medium-symbolic";
    }

    return "audio-volume-high-symbolic";
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
