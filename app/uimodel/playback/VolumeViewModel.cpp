// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/rt/PlaybackService.h"
#include <ao/uimodel/playback/VolumeViewModel.h>

#include <algorithm>
#include <functional>
#include <utility>

namespace ao::uimodel::playback
{
  VolumeViewModel::VolumeViewModel(rt::PlaybackService& playback, std::function<void(VolumeViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    auto const refreshCallback = [this] { refresh(); };
    _outputSub = _playback.onOutputChanged([refreshCallback](auto const&) { refreshCallback(); });
    _startedSub = _playback.onStarted(refreshCallback);
    _volumeSub = _playback.onVolumeChanged([refreshCallback](float) { refreshCallback(); });
    refresh();
  }

  void VolumeViewModel::handleVolumeChanged(float volume)
  {
    _playback.setVolume(volume);
  }

  void VolumeViewModel::refresh()
  {
    auto const& state = _playback.state();

    auto view = VolumeViewState{};
    view.visible = state.volumeAvailable;
    view.volume = state.volume;

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
    constexpr float kScrollStep = 0.05F;
    float const delta = (scrollDy > 0) ? -kScrollStep : kScrollStep;
    return std::clamp(currentVolume + delta, 0.0F, 1.0F);
  }
} // namespace ao::uimodel::playback
