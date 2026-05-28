// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Types.h>
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/PlaybackTimeViewModel.h>

#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace ao::uimodel::playback
{
  namespace
  {
    bool isAdvancingTransport(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Playing || transport == audio::Transport::Buffering ||
             transport == audio::Transport::Seeking;
    }
  } // namespace

  PlaybackTimeViewModel::PlaybackTimeViewModel(rt::PlaybackService& playback,
                                               std::function<void(PlaybackTimeViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    auto const refreshImmediate = [this] { refresh(true, false); };

    _startedSub = _playback.onStarted(refreshImmediate);
    _pausedSub = _playback.onPaused(refreshImmediate);

    _idleSub = _playback.onIdle(refreshImmediate);
    _stoppedSub = _playback.onStopped(refreshImmediate);
    _preparingSub = _playback.onPreparing(refreshImmediate);

    _seekUpdateSub = _playback.onSeekUpdate(
      [this](rt::PlaybackService::SeekUpdate const& ev)
      {
        bool const isPreview = ev.mode == rt::PlaybackService::SeekMode::Preview;

        if (!isPreview)
        {
          refresh(true, false, ev.positionMs);
        }
        else
        {
          refresh(false, true, ev.positionMs);
        }
      });

    refresh(true, false);
  }

  void PlaybackTimeViewModel::refresh(bool immediateUpdate,
                                      bool isPreviewing,
                                      std::optional<std::uint32_t> optOverridePosition)
  {
    auto const& state = _playback.state();

    auto view = PlaybackTimeViewState{};
    view.durationMs = state.durationMs;
    view.positionMs = optOverridePosition.value_or(state.positionMs);
    view.isPlaying = isAdvancingTransport(state.transport);
    view.isPreviewing = isPreviewing;
    view.immediateUpdate = immediateUpdate;

    if (_onRender)
    {
      _onRender(view);
    }
  }

  std::string PlaybackTimeViewModel::describeTimeTemplate(PlaybackTimeMode mode)
  {
    switch (mode)
    {
      case PlaybackTimeMode::Elapsed:
      case PlaybackTimeMode::Duration: return "00:00";
      case PlaybackTimeMode::Default:
      default: return "00:00 / 00:00";
    }
  }

  std::string PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode mode, std::uint32_t posMs, std::uint32_t durMs)
  {
    constexpr int kMsInSec = 1000;
    constexpr int kSecInMin = 60;

    auto const posSec = posMs / kMsInSec;

    switch (auto const durSec = durMs / kMsInSec; mode)
    {
      case PlaybackTimeMode::Elapsed: return std::format("{:d}:{:02d}", posSec / kSecInMin, posSec % kSecInMin);

      case PlaybackTimeMode::Duration: return std::format("{:d}:{:02d}", durSec / kSecInMin, durSec % kSecInMin);

      case PlaybackTimeMode::Default:
      default:
        return std::format(
          "{:d}:{:02d} / {:d}:{:02d}", posSec / kSecInMin, posSec % kSecInMin, durSec / kSecInMin, durSec % kSecInMin);
    }
  }
} // namespace ao::uimodel::playback
