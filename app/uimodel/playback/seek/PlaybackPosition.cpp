// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/seek/PlaybackPosition.h>

#include <ao/audio/Transport.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

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
    , _clockTransport{playback.snapshot().transport.transport}
    , _clockOccurrenceId{playback.snapshot().transport.occurrenceId}
    , _clockPositionRevision{playback.snapshot().transport.positionRevision}
    , _clockDuration{playback.snapshot().transport.duration}
  {
    if (!_onRender)
    {
      return;
    }

    _snapshotSub =
      _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { onSnapshotChanged(snapshot); });
    _seekPreviewSub = _playback.events().onSeekPreview([this](std::chrono::milliseconds const elapsed)
                                                       { refresh(false, true, elapsed); });

    refresh(true);
  }

  void PlaybackPositionViewModel::onSnapshotChanged(rt::PlaybackSnapshot const& snapshot)
  {
    if (snapshot.transport.transport == _clockTransport && snapshot.transport.occurrenceId == _clockOccurrenceId &&
        snapshot.transport.positionRevision == _clockPositionRevision && snapshot.transport.duration == _clockDuration)
    {
      return;
    }

    _clockTransport = snapshot.transport.transport;
    _clockOccurrenceId = snapshot.transport.occurrenceId;
    _clockPositionRevision = snapshot.transport.positionRevision;
    _clockDuration = snapshot.transport.duration;

    render(snapshot.transport, true, false);
  }

  void PlaybackPositionViewModel::seekPreview(rt::PlaybackOccurrenceId const expectedOccurrenceId,
                                              std::chrono::milliseconds const elapsed)
  {
    _commands.seek(expectedOccurrenceId, elapsed, rt::PlaybackSeekMode::Preview);
  }

  void PlaybackPositionViewModel::seekFinal(rt::PlaybackOccurrenceId const expectedOccurrenceId,
                                            std::chrono::milliseconds const elapsed)
  {
    _commands.seek(expectedOccurrenceId, elapsed);
  }

  void PlaybackPositionViewModel::seekBy(std::chrono::milliseconds const delta)
  {
    _commands.seekBy(_playback.snapshot().transport.occurrenceId, delta);
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
    view.occurrenceId = state.occurrenceId;
    view.duration = state.duration;
    view.elapsed = optOverrideElapsed.value_or(state.elapsed);
    view.isPlaying = isAdvancingTransport(state.transport);
    view.seekable = state.duration > std::chrono::milliseconds{0};
    view.isPreviewing = isPreviewing;
    view.immediateUpdate = immediateUpdate;

    _onRender(view);
  }
} // namespace ao::uimodel
