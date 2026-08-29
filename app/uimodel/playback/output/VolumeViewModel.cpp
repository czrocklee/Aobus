// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/output/VolumeViewModel.h>

#include <ao/Contract.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/output/PlaybackOutputText.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>

namespace ao::uimodel
{
  VolumeViewModel::VolumeViewModel(rt::PlaybackService& playback)
    : _playback{playback}, _commands{playback.commands()}, _lastVolume{playback.snapshot().transport.volume}
  {
  }

  VolumeViewModel::VolumeViewModel(rt::PlaybackService& playback,
                                   i18n::MessageCatalog const& textCatalog,
                                   std::function<void(VolumeViewState const&)> onRender)
    : VolumeViewModel{playback}
  {
    _optTextCatalog = textCatalog;
    _onRender = std::move(onRender);

    if (!_onRender)
    {
      return;
    }

    _snapshotSub =
      _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { handleSnapshot(snapshot); });
    refresh();
  }

  void VolumeViewModel::handleVolumeChanged(float volume)
  {
    _commands.setVolume(volume);
  }

  void VolumeViewModel::handleMutedChanged(bool muted)
  {
    _commands.setMuted(muted);
  }

  void VolumeViewModel::toggleMuted()
  {
    _commands.setMuted(!_playback.snapshot().transport.volume.muted);
  }

  void VolumeViewModel::handleScroll(double scrollDy)
  {
    auto const& volume = _playback.snapshot().transport.volume;
    applyVolumeTarget(volume.level, volume.muted, resolveVolumeScroll(volume.level, scrollDy));
  }

  void VolumeViewModel::adjustVolume(float const delta)
  {
    auto const& volume = _playback.snapshot().transport.volume;
    applyVolumeTarget(volume.level, volume.muted, volume.level + delta);
  }

  void VolumeViewModel::applyVolumeTarget(float const currentVolume, bool const muted, float const targetVolume)
  {
    auto const newVolume = std::clamp(targetVolume, 0.0F, 1.0F);

    if (muted && newVolume > currentVolume)
    {
      _commands.setMuted(false);
    }

    _commands.setVolume(newVolume);
  }

  void VolumeViewModel::refresh()
  {
    auto const& volume = _playback.snapshot().transport.volume;
    _lastVolume = volume;
    render(volume);
  }

  void VolumeViewModel::handleSnapshot(rt::PlaybackSnapshot const& snapshot)
  {
    if (snapshot.transport.volume == _lastVolume)
    {
      return;
    }

    _lastVolume = snapshot.transport.volume;
    render(snapshot.transport.volume);
  }

  void VolumeViewModel::render(rt::VolumeState const& volume)
  {
    if (!_onRender)
    {
      return;
    }

    AO_INVARIANT(_optTextCatalog, "Volume presentation requires a text catalog");

    auto view = VolumeViewState{
      .visible = volume.available,
      .volume = volume.level,
      .isHardwareAssisted = volume.hardwareAssisted,
      .muted = volume.muted,
      .indicatorKind = resolveIndicatorKind(volume.level, volume.muted),
      .tooltip = volumeTooltip(*_optTextCatalog,
                               static_cast<std::int32_t>(std::round(volume.level * 100.0F)),
                               volume.muted,
                               volume.hardwareAssisted),
    };

    _onRender(view);
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
} // namespace ao::uimodel
