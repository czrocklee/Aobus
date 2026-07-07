// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <functional>
#include <utility>

namespace ao::uimodel
{
  SoulAura resolveSoulAura(bool const playing, bool const ready, rt::QualityState const& signal) noexcept
  {
    if (!playing)
    {
      return SoulAura::Dormant;
    }

    if (!ready)
    {
      return SoulAura::Veiled;
    }

    if (signal.overall == audio::Quality::Clipped || signal.pipelineQuality == audio::Quality::Clipped)
    {
      return SoulAura::Burning;
    }

    if (signal.pipelineQuality == audio::Quality::LinearIntervention)
    {
      return SoulAura::Turbulent;
    }

    if (!signal.fullyVerified || signal.sourceQuality == audio::Quality::LossySource ||
        signal.sourceQuality == audio::Quality::Unknown)
    {
      return SoulAura::Veiled;
    }

    switch (signal.pipelineQuality)
    {
      case audio::Quality::BitwisePerfect: return SoulAura::Radiant;
      case audio::Quality::LosslessPadded:
      case audio::Quality::LosslessFloat: return SoulAura::Flowing;
      case audio::Quality::LinearIntervention: return SoulAura::Turbulent;
      case audio::Quality::Clipped: return SoulAura::Burning;
      case audio::Quality::LossySource:
      case audio::Quality::Unknown: return SoulAura::Veiled;
    }

    return SoulAura::Veiled;
  }

  AobusSoulViewModel::AobusSoulViewModel(rt::PlaybackService& playback,
                                         std::function<void(AobusSoulViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    auto const refreshCallback = [this] { refresh(); };

    _qualitySub = _playback.onQualityChanged([refreshCallback](auto const&) { refreshCallback(); });
    _outputDeviceSub = _playback.onOutputDeviceChanged([refreshCallback](auto const&) { refreshCallback(); });
    _startedSub = _playback.onStarted(refreshCallback);

    _stoppedSub = _playback.onStopped(refreshCallback);
    _idleSub = _playback.onIdle(refreshCallback);

    refresh();
  }

  void AobusSoulViewModel::refresh()
  {
    auto const& state = _playback.state();
    bool const playing = (state.transport == audio::Transport::Playing);

    auto view = AobusSoulViewState{};
    view.isBreathing = playing;
    view.aura = resolveSoulAura(playing, state.ready, state.quality);

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
