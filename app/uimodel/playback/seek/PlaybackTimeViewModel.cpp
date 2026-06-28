// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Types.h>
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/seek/PlaybackTimeViewModel.h>

#include <chrono>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace ao::uimodel
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
          refresh(true, false, ev.elapsed);
        }
        else
        {
          refresh(false, true, ev.elapsed);
        }
      });

    refresh(true, false);
  }

  void PlaybackTimeViewModel::refresh(bool immediateUpdate,
                                      bool isPreviewing,
                                      std::optional<std::chrono::milliseconds> optOverrideElapsed)
  {
    auto const& state = _playback.state();

    auto view = PlaybackTimeViewState{};
    view.duration = state.duration;
    view.elapsed = optOverrideElapsed.value_or(state.elapsed);
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

  std::string PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode mode,
                                                        std::chrono::milliseconds elapsed,
                                                        std::chrono::milliseconds duration)
  {
    auto const elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

    switch (auto const durSec = std::chrono::duration_cast<std::chrono::seconds>(duration).count(); mode)
    {
      case PlaybackTimeMode::Elapsed: return std::format("{:d}:{:02d}", elapsedSec / 60, elapsedSec % 60);

      case PlaybackTimeMode::Duration: return std::format("{:d}:{:02d}", durSec / 60, durSec % 60);

      case PlaybackTimeMode::Default:
      default:
        return std::format("{:d}:{:02d} / {:d}:{:02d}", elapsedSec / 60, elapsedSec % 60, durSec / 60, durSec % 60);
    }
  }
} // namespace ao::uimodel
