// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Types.h>
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/SeekViewModel.h>

#include <cstdint>
#include <functional>
#include <optional>
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

  SeekViewModel::SeekViewModel(rt::PlaybackService& playback, std::function<void(SeekViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    auto const refreshImmediate = [this] { refresh(true); };

    _startedSub = _playback.onStarted(refreshImmediate);
    _pausedSub = _playback.onPaused(refreshImmediate);

    _idleSub = _playback.onIdle(refreshImmediate);
    _stoppedSub = _playback.onStopped(refreshImmediate);
    _preparingSub = _playback.onPreparing(refreshImmediate);

    _seekUpdateSub = _playback.onSeekUpdate(
      [this](rt::PlaybackService::SeekUpdate const& ev)
      {
        if (ev.mode == rt::PlaybackService::SeekMode::Final)
        {
          refresh(true, ev.positionMs);
        }
        else if (ev.mode == rt::PlaybackService::SeekMode::Preview)
        {
          refresh(false, ev.positionMs);
        }
      });

    refresh(true);
  }

  void SeekViewModel::seekPreview(std::uint32_t positionMs)
  {
    _playback.seek(positionMs, rt::PlaybackService::SeekMode::Preview);
  }

  void SeekViewModel::seekFinal(std::uint32_t positionMs)
  {
    _playback.seek(positionMs, rt::PlaybackService::SeekMode::Final);
  }

  void SeekViewModel::refresh(bool immediateUpdate, std::optional<std::uint32_t> optOverridePosition)
  {
    auto const& state = _playback.state();

    auto view = SeekViewState{};
    view.durationMs = state.durationMs;
    view.positionMs = optOverridePosition.value_or(state.positionMs);
    view.isPlaying = isAdvancingTransport(state.transport);
    view.enabled = state.durationMs > 0;
    view.immediateUpdate = immediateUpdate;

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel::playback
