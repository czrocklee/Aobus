// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Transport.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/seek/PlaybackPositionViewModel.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
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

  PlaybackPositionViewModel::PlaybackPositionViewModel(rt::PlaybackService& playback,
                                                       std::function<void(PlaybackPositionViewState const&)> onRender)
    : _playback{playback}
    , _commands{playback.commands()}
    , _onRender{std::move(onRender)}
    , _clockChangeFilter{playback.snapshot().transport}
  {
    if (!_onRender)
    {
      return;
    }

    _snapshotSub = _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) noexcept
                                                 { onSnapshotChanged(snapshot); });
    _seekPreviewSub = _playback.events().onSeekPreview([this](std::chrono::milliseconds const elapsed) noexcept
                                                       { refresh(false, true, elapsed); });

    refresh(true);
  }

  void PlaybackPositionViewModel::onSnapshotChanged(rt::PlaybackSnapshot const& snapshot)
  {
    if (!_clockChangeFilter.update(snapshot.transport))
    {
      return;
    }

    render(snapshot.transport, true, false);
  }

  void PlaybackPositionViewModel::seekPreview(std::chrono::milliseconds elapsed)
  {
    _commands.seek(elapsed, rt::PlaybackSeekMode::Preview);
  }

  void PlaybackPositionViewModel::seekFinal(std::chrono::milliseconds elapsed)
  {
    _commands.seek(elapsed, rt::PlaybackSeekMode::Final);
  }

  void PlaybackPositionViewModel::seekBy(std::chrono::milliseconds const delta)
  {
    auto const& state = _playback.snapshot().transport;

    if (state.duration <= std::chrono::milliseconds{0})
    {
      return;
    }

    auto const elapsed = std::clamp(state.elapsed, std::chrono::milliseconds{0}, state.duration);
    auto const clampedDelta = std::clamp(delta, -elapsed, state.duration - elapsed);
    seekFinal(elapsed + clampedDelta);
  }

  void PlaybackPositionViewModel::refresh(bool immediateUpdate,
                                          bool isPreviewing,
                                          std::optional<std::chrono::milliseconds> optOverrideElapsed)
  {
    render(_playback.snapshot().transport, immediateUpdate, isPreviewing, optOverrideElapsed);
  }

  void PlaybackPositionViewModel::render(rt::PlaybackTransportSnapshot const& state,
                                         bool const immediateUpdate,
                                         bool const isPreviewing,
                                         std::optional<std::chrono::milliseconds> const optOverrideElapsed)
  {
    if (!_onRender)
    {
      return;
    }

    auto view = PlaybackPositionViewState{};
    view.duration = state.duration;
    view.elapsed = optOverrideElapsed.value_or(state.elapsed);
    view.isPlaying = isAdvancingTransport(state.transport);
    view.seekable = state.duration > std::chrono::milliseconds{0};
    view.isPreviewing = isPreviewing;
    view.immediateUpdate = immediateUpdate;

    _onRender(view);
  }
} // namespace ao::uimodel
