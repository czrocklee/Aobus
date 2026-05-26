// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/audio/Backend.h"
#include "ao/audio/Types.h"
#include "ao/rt/PlaybackService.h"
#include <ao/uimodel/playback/AobusSoulViewModel.h>

#include <functional>
#include <string>
#include <utility>

namespace ao::uimodel::playback
{
  namespace
  {
    std::string const kColorCyan{"#00E5FF"};
    std::string const kColorGray{"#6B7280"};
    std::string const kColorPurple{"#A855F7"};
    std::string const kColorGreen{"#10B981"};
    std::string const kColorOrange{"#F59E0B"};
    std::string const kColorRed{"#EF4444"};

    std::string colorForQuality(audio::Quality const quality)
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

    std::string computeColor(bool const playing, bool const ready, audio::Quality const quality)
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

  AobusSoulViewModel::AobusSoulViewModel(rt::PlaybackService& playback,
                                         std::function<void(AobusSoulViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    auto const refreshCallback = [this] { refresh(); };

    _qualitySub = _playback.onQualityChanged([refreshCallback](auto const&) { refreshCallback(); });
    _outputSub = _playback.onOutputChanged([refreshCallback](auto const&) { refreshCallback(); });
    _startedSub = _playback.onStarted(refreshCallback);

    auto const onStopped = [this, refreshCallback] { refreshCallback(); };

    _stoppedSub = _playback.onStopped(onStopped);
    _idleSub = _playback.onIdle(onStopped);

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
} // namespace ao::uimodel::playback
