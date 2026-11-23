// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/seek/PlaybackPosition.h>

#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <chrono>
#include <functional>
#include <memory>
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

  PlaybackPositionViewState playbackPositionView(rt::PlaybackTransportSnapshot const& state) noexcept
  {
    if (state.nowPlaying.trackId == kInvalidTrackId)
    {
      return {};
    }

    return {.occurrenceId = state.occurrenceId,
            .duration = state.duration,
            .elapsed = state.elapsed,
            .hasTrack = true,
            .isPlaying = isAdvancingTransport(state.transport),
            .seekable = state.hasSeekableSubject()};
  }

  PlaybackPositionViewModel::PlaybackPositionViewModel(rt::PlaybackService& playback,
                                                       std::function<void(PlaybackPositionViewState const&)> onRender)
    : _playback{playback}
    , _commands{playback.commands()}
    , _onRender{std::move(onRender)}
    , _clockTransport{playback.snapshot().transport.transport}
    , _clockTrackId{playback.snapshot().transport.nowPlaying.trackId}
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
    _seekPreviewEndedSub = _playback.events().onSeekPreviewEnded(
      [this](rt::PlaybackOccurrenceId const occurrenceId)
      {
        if (occurrenceId == _clockOccurrenceId)
        {
          refresh(true, false, _playback.elapsed());
        }
      });

    refresh(true);
  }

  void PlaybackPositionViewModel::onSnapshotChanged(rt::PlaybackSnapshot const& snapshot)
  {
    if (snapshot.transport.transport == _clockTransport && snapshot.transport.nowPlaying.trackId == _clockTrackId &&
        snapshot.transport.occurrenceId == _clockOccurrenceId &&
        snapshot.transport.positionRevision == _clockPositionRevision && snapshot.transport.duration == _clockDuration)
    {
      return;
    }

    if (_pendingSliderOccurrencePtr &&
        (*_pendingSliderOccurrencePtr != snapshot.transport.occurrenceId || !snapshot.transport.hasSeekableSubject()))
    {
      _pendingSliderOccurrencePtr.reset();
    }

    _clockTransport = snapshot.transport.transport;
    _clockTrackId = snapshot.transport.nowPlaying.trackId;
    _clockOccurrenceId = snapshot.transport.occurrenceId;
    _clockPositionRevision = snapshot.transport.positionRevision;
    _clockDuration = snapshot.transport.duration;

    render(snapshot.transport, true, false);
  }

  void PlaybackPositionViewModel::seekPreview(rt::PlaybackOccurrenceId const expectedOccurrenceId,
                                              std::chrono::milliseconds const elapsed)
  {
    if (_pointerSeek.serial == 0 || _pointerSeek.occurrenceId != expectedOccurrenceId)
    {
      _pointerSeek = _commands.beginPointerSeek(expectedOccurrenceId);
    }

    _commands.seekPointer(_pointerSeek, elapsed, rt::PlaybackSeekMode::Preview);
  }

  void PlaybackPositionViewModel::seekFinal(rt::PlaybackOccurrenceId const expectedOccurrenceId,
                                            std::chrono::milliseconds const elapsed)
  {
    submitFinal(expectedOccurrenceId, elapsed, {});
  }

  void PlaybackPositionViewModel::cancelPointerSeek()
  {
    if (_pointerSeek.serial != 0)
    {
      _commands.cancelPointerSeek(std::exchange(_pointerSeek, {}));
    }
  }

  void PlaybackPositionViewModel::submitFinal(rt::PlaybackOccurrenceId const occurrenceId,
                                              std::chrono::milliseconds const elapsed,
                                              compat::MoveOnlyFunction<void()> onSettled)
  {
    // A discrete change may replace a pointer release in the frontend debounce. Its Final
    // still closes that gesture's shared preview, as well as settling the discrete thumb.
    if (_pointerSeek.serial != 0 && _pointerSeek.occurrenceId == occurrenceId)
    {
      _commands.seekPointer(
        std::exchange(_pointerSeek, {}), elapsed, rt::PlaybackSeekMode::Final, std::move(onSettled));
      return;
    }

    _commands.seek(occurrenceId, elapsed, rt::PlaybackSeekMode::Final, std::move(onSettled));
  }

  void PlaybackPositionViewModel::commitSliderUpdate(SeekSliderUpdate const& update)
  {
    if (update.action != SeekSliderAction::Commit)
    {
      return;
    }

    if (update.endsPointerPreview)
    {
      seekFinal(update.occurrenceId, update.elapsed);
      return;
    }

    // A new submission (including one for the same occurrence) supersedes
    // the previous hold. Queued callbacks must not keep this control alive.
    _pendingSliderOccurrencePtr = std::make_shared<rt::PlaybackOccurrenceId const>(update.occurrenceId);
    submitFinal(update.occurrenceId,
                update.elapsed,
                [this, requestPtr = std::weak_ptr{_pendingSliderOccurrencePtr}]
                {
                  if (!requestPtr.expired())
                  {
                    _pendingSliderOccurrencePtr.reset();
                    render(_playback.snapshot().transport, true, false, _playback.elapsed());
                  }
                });
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

    auto view = playbackPositionView(state);

    if (view.seekable && optOverrideElapsed)
    {
      view.elapsed = *optOverrideElapsed;
    }

    view.isPreviewing = view.seekable && isPreviewing;
    view.immediateUpdate = immediateUpdate;

    _onRender(view);
  }
} // namespace ao::uimodel
