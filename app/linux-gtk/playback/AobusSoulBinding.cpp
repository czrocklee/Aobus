// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AobusSoulBinding.h"
#include "app/AobusSoul.h"
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>

namespace ao::gtk
{
  namespace
  {
    Gdk::RGBA const kColorCyan{"#00E5FF"};
    Gdk::RGBA const kColorGray{"#6B7280"};
    Gdk::RGBA const kColorPurple{"#A855F7"};
    Gdk::RGBA const kColorGreen{"#10B981"};
    Gdk::RGBA const kColorOrange{"#F59E0B"};
    Gdk::RGBA const kColorRed{"#EF4444"};

    Gdk::RGBA colorForQuality(audio::Quality const quality)
    {
      switch (quality)
      {
        case audio::Quality::BitwisePerfect:
        case audio::Quality::LosslessPadded: return kColorPurple;
        case audio::Quality::LosslessFloat: return kColorGreen;
        case audio::Quality::LinearIntervention: return kColorOrange;
        case audio::Quality::Clipped: return kColorRed;
        case audio::Quality::LossySource:
        case audio::Quality::Unknown: return kColorGray;
      }
      return kColorGray;
    }

    Gdk::RGBA computeColor(bool const playing, bool const ready, audio::Quality const quality)
    {
      if (!playing)
      {
        return kColorCyan;
      }

      if (!ready)
      {
        return kColorGray;
      }

      return colorForQuality(quality);
    }
  } // namespace

  AobusSoulBinding::AobusSoulBinding(AobusSoul& soul, rt::PlaybackService& playback)
    : _soul{soul}, _playback{playback}
  {
    _qualitySub = _playback.onQualityChanged(
      [this](auto const& ev)
      {
        _quality = ev.quality;
        _ready = ev.ready;
        _soul.setAura(computeColor(_playing, _ready, _quality));
      });

    _outputSub = _playback.onOutputChanged(
      [this](auto const&)
      {
        _ready = _playback.state().ready;
        _soul.setAura(computeColor(_playing, _ready, _quality));
      });

    _startedSub = _playback.onStarted(
      [this]
      {
        _playing = true;
        _soul.breathe(true);
        _soul.setAura(computeColor(true, _ready, _quality));
      });

    auto const onStopped = [this]
    {
      _playing = false;
      _soul.breathe(false);
      _soul.setAura(computeColor(false, _ready, _quality));
    };

    _stoppedSub = _playback.onStopped(onStopped);
    _idleSub = _playback.onIdle(onStopped);

    // Sync initial state
    auto const& state = _playback.state();
    _playing = (state.transport == audio::Transport::Playing);
    _ready = state.ready;
    _quality = state.quality;
    _soul.breathe(_playing);
    _soul.setAura(computeColor(_playing, _ready, _quality));
  }

  AobusSoulBinding::~AobusSoulBinding()
  {
    _qualitySub.reset();
    _outputSub.reset();
    _startedSub.reset();
    _stoppedSub.reset();
    _idleSub.reset();
  }
} // namespace ao::gtk
