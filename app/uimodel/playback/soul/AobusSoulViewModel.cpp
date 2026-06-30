// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <functional>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    AuraColor colorForQuality(audio::Quality const quality)
    {
      switch (quality)
      {
        case audio::Quality::BitwisePerfect:
        case audio::Quality::LosslessPadded: return AuraColor::Perfect;
        case audio::Quality::LosslessFloat: return AuraColor::Lossless;
        case audio::Quality::LinearIntervention: return AuraColor::Intervention;
        case audio::Quality::Clipped: return AuraColor::Clipped;
        case audio::Quality::LossySource:
        case audio::Quality::Unknown: return AuraColor::Unknown;
      }

      return AuraColor::Unknown;
    }

    AuraColor computeColor(bool const playing, bool const ready, audio::Quality const quality)
    {
      if (!playing)
      {
        return AuraColor::Idle;
      }

      if (!ready)
      {
        return AuraColor::Unknown;
      }

      return colorForQuality(quality);
    }
  } // namespace

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
    view.auraColor = computeColor(playing, state.ready, state.quality);

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
