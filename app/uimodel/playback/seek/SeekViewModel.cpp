// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Transport.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/seek/SeekViewModel.h>

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

  SeekViewModel::SeekViewModel(rt::PlaybackService& playback, std::function<void(SeekViewState const&)> onRender)
    : _playback{playback}
    , _commands{playback.commands()}
    , _onRender{std::move(onRender)}
    , _clockChangeFilter{playback.snapshot().transport}
  {
    _snapshotSub =
      _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { onSnapshotChanged(snapshot); });
    _seekPreviewSub = _playback.events().onSeekPreview([this](rt::PlaybackSeekPreview const& preview)
                                                       { refresh(false, preview.elapsed); });

    refresh(true);
  }

  void SeekViewModel::onSnapshotChanged(rt::PlaybackSnapshot const& snapshot)
  {
    if (!_clockChangeFilter.update(snapshot.transport))
    {
      return;
    }

    render(snapshot.transport, true);
  }

  void SeekViewModel::seekPreview(std::chrono::milliseconds elapsed)
  {
    _commands.seek(elapsed, rt::PlaybackSeekMode::Preview);
  }

  void SeekViewModel::seekFinal(std::chrono::milliseconds elapsed)
  {
    _commands.seek(elapsed, rt::PlaybackSeekMode::Final);
  }

  void SeekViewModel::seekBy(std::chrono::milliseconds const delta)
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

  void SeekViewModel::refresh(bool immediateUpdate, std::optional<std::chrono::milliseconds> optOverrideElapsed)
  {
    render(_playback.snapshot().transport, immediateUpdate, optOverrideElapsed);
  }

  void SeekViewModel::render(rt::PlaybackTransportSnapshot const& state,
                             bool const immediateUpdate,
                             std::optional<std::chrono::milliseconds> const optOverrideElapsed)
  {
    auto view = SeekViewState{};
    view.duration = state.duration;
    view.elapsed = optOverrideElapsed.value_or(state.elapsed);
    view.isPlaying = isAdvancingTransport(state.transport);
    view.enabled = state.duration > std::chrono::milliseconds{0};
    view.immediateUpdate = immediateUpdate;

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
