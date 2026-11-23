// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/command/PlaybackActions.h>

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    enum class PlayIntent : std::uint8_t
    {
      None,
      Resume,
      StartSelection,
    };

    PlayIntent playIntent(rt::PlaybackTransportSnapshot const& transport) noexcept
    {
      // A pending route blocks both resume and a new start. Never substitute a browsed track for a held one.
      if (transport.output.optPendingOutputSelection)
      {
        return PlayIntent::None;
      }

      if (transport.transport == audio::Transport::Paused)
      {
        return PlayIntent::Resume;
      }

      if (!transport.ready)
      {
        return PlayIntent::None;
      }

      if (transport.transport == audio::Transport::Idle)
      {
        return transport.hasDeferredResume ? PlayIntent::Resume : PlayIntent::StartSelection;
      }

      return transport.transport == audio::Transport::Error ? PlayIntent::StartSelection : PlayIntent::None;
    }

    bool isActivePlayback(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Opening || transport == audio::Transport::Buffering ||
             transport == audio::Transport::Playing || transport == audio::Transport::Seeking;
    }

    bool canStop(rt::PlaybackTransportSnapshot const& transport) noexcept
    {
      return isActivePlayback(transport.transport) || transport.transport == audio::Transport::Paused ||
             transport.transport == audio::Transport::Error;
    }

    constexpr std::size_t commandIndex(PlaybackCommand const command) noexcept
    {
      return static_cast<std::size_t>(command);
    }

    static_assert(commandIndex(PlaybackCommand::CycleRepeat) < 8);

    rt::RepeatMode nextRepeatMode(rt::RepeatMode mode) noexcept
    {
      switch (mode)
      {
        case rt::RepeatMode::Off: return rt::RepeatMode::All;
        case rt::RepeatMode::All: return rt::RepeatMode::One;
        case rt::RepeatMode::One: return rt::RepeatMode::Off;
      }

      return rt::RepeatMode::Off;
    }
  } // namespace

  struct PlaybackActions::FocusedSelection final
  {
    FocusedSelection(PlaybackActions& ownerValue, rt::WorkspaceService& workspaceValue, rt::ViewService& viewsValue)
      : owner{ownerValue}, workspace{workspaceValue}, views{viewsValue}
    {
      workspaceSub = workspace.onChanged([this](auto const&) { bindProjection(); });
      projectionSub = views.onProjectionChanged(
        [this](auto const& change)
        {
          if (change.viewId == workspace.snapshot().activeViewId)
          {
            bindProjection();
          }
        });
      selectionSub = views.onSelectionChanged(
        [this](auto const& change)
        {
          if (change.viewId == workspace.snapshot().activeViewId)
          {
            refresh();
          }
        });
    }

    bool hasVisibleSelection() const
    {
      return views.firstVisibleSelectedTrack(workspace.snapshot().activeViewId) != kInvalidTrackId;
    }

    void refresh() { owner.setSelectionAvailability(hasVisibleSelection(), owner.hasSystemMediaSelection()); }

    void bindProjection()
    {
      auto projectionRes = views.findTrackListProjection(workspace.snapshot().activeViewId);
      auto currentPtr = projectionRes ? std::move(*projectionRes) : nullptr;

      if (currentPtr != projectionPtr)
      {
        deltaSub.reset();
        projectionPtr = std::move(currentPtr);

        if (projectionPtr)
        {
          // subscribe delivers its initial reset synchronously. Publish that
          // state below, only after installing the subscription: an observer
          // can navigate (and thus replace this binding) from refresh().
          deltaSub = projectionPtr->subscribe(
            [this](auto const&)
            {
              if (deltaSub)
              {
                refresh();
              }
            });
        }
      }

      refresh();
    }

    PlaybackActions& owner;
    rt::WorkspaceService& workspace;
    rt::ViewService& views;
    std::shared_ptr<rt::TrackListProjection const> projectionPtr;
    async::Subscription workspaceSub;
    async::Subscription projectionSub;
    async::Subscription selectionSub;
    async::Subscription deltaSub;
  };

  PlaybackActions::PlaybackActions(rt::PlaybackService& playback,
                                   std::function<void()> playSelection,
                                   SystemMediaPlayHandler systemMediaPlay)
    : _playback{playback}
    , _commands{playback.commands()}
    , _playSelection{std::move(playSelection)}
    , _systemMediaPlay{std::move(systemMediaPlay)}
    , _lastSnapshot{playback.snapshot()}
  {
    _snapshotSub =
      _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { handleSnapshot(snapshot); });
  }

  PlaybackActions::~PlaybackActions() = default;

  void PlaybackActions::bindFocusedSelection(rt::WorkspaceService& workspace, rt::ViewService& views)
  {
    _focusedSelectionPtr = std::make_unique<FocusedSelection>(*this, workspace, views);
    _focusedSelectionPtr->bindProjection();
  }

  bool PlaybackActions::hasSystemMediaSelection() const
  {
    if (_systemMediaPlay.play)
    {
      return _systemMediaPlay.canPlay ? _systemMediaPlay.canPlay() : true;
    }

    return _focusedSelectionPtr ? _focusedSelectionPtr->hasVisibleSelection() : _selectionAvailable;
  }

  void PlaybackActions::setSelectionAvailability(bool const primaryAvailable, bool const systemMediaAvailable)
  {
    auto const primaryChanged = std::exchange(_selectionAvailable, primaryAvailable) != primaryAvailable;
    auto const systemMediaChanged =
      std::exchange(_systemMediaSelectionAvailable, systemMediaAvailable) != systemMediaAvailable;

    if (primaryChanged || systemMediaChanged)
    {
      _commandAvailabilityChangedSignals[commandIndex(PlaybackCommand::Play)].emit();
      _commandAvailabilityChangedSignals[commandIndex(PlaybackCommand::PlayPause)].emit();
      _availabilityChangedSignal.emit();
    }
  }

  void PlaybackActions::setSelectionAvailable(bool const available)
  {
    setSelectionAvailability(available, _systemMediaPlay.play ? hasSystemMediaSelection() : available);
  }

  bool PlaybackActions::tryExecute(PlaybackCommand command, PlaybackActionOrigin const origin)
  {
    if (!isEnabled(command, origin))
    {
      return false;
    }

    auto const& snapshot = _playback.snapshot();
    auto const& transport = snapshot.transport;

    switch (auto const& succession = snapshot.succession; command)
    {
      case PlaybackCommand::Play:
      case PlaybackCommand::PlayPause:
        if (command == PlaybackCommand::PlayPause && isActivePlayback(transport.transport))
        {
          _commands.pause();
        }
        else if (playIntent(transport) == PlayIntent::Resume)
        {
          _commands.resume();
        }
        else if (origin == PlaybackActionOrigin::SystemMedia && _systemMediaPlay.play)
        {
          return _systemMediaPlay.play();
        }
        else if (_playSelection)
        {
          _playSelection();
        }

        break;

      case PlaybackCommand::Pause: _commands.pause(); break;

      case PlaybackCommand::Stop: _commands.stop(); break;

      case PlaybackCommand::Next: _commands.next(); break;

      case PlaybackCommand::Previous: _commands.previous(); break;

      case PlaybackCommand::ToggleShuffle:
        _commands.setShuffleMode(succession.shuffle == rt::ShuffleMode::Off ? rt::ShuffleMode::On
                                                                            : rt::ShuffleMode::Off);
        break;

      case PlaybackCommand::CycleRepeat: _commands.setRepeatMode(nextRepeatMode(succession.repeat)); break;
    }

    return true;
  }

  bool PlaybackActions::isEnabled(PlaybackCommand command, PlaybackActionOrigin const origin) const
  {
    auto const& snapshot = _playback.snapshot();
    auto const& transport = snapshot.transport;
    auto const playEnabled = [&]
    {
      if (auto const intent = playIntent(transport); intent != PlayIntent::StartSelection)
      {
        return intent == PlayIntent::Resume;
      }

      // A command can arrive before our focus or projection subscriber refreshes the presentation.
      if (origin == PlaybackActionOrigin::SystemMedia && _systemMediaPlay.play)
      {
        return hasSystemMediaSelection();
      }

      return _focusedSelectionPtr ? _focusedSelectionPtr->hasVisibleSelection() : _selectionAvailable;
    };

    switch (auto const& succession = snapshot.succession; command)
    {
      case PlaybackCommand::Play: return playEnabled();
      case PlaybackCommand::Pause: return isActivePlayback(transport.transport);
      case PlaybackCommand::PlayPause: return isActivePlayback(transport.transport) || playEnabled();
      case PlaybackCommand::Stop: return canStop(transport);
      case PlaybackCommand::Next: return transport.ready && succession.hasNext;
      case PlaybackCommand::Previous: return transport.ready && succession.hasPrevious;
      case PlaybackCommand::ToggleShuffle:
      case PlaybackCommand::CycleRepeat: return transport.ready;
    }

    return false;
  }

  bool PlaybackActions::isCapable(PlaybackCommand command) const
  {
    switch (auto const& transport = _playback.snapshot().transport; command)
    {
      case PlaybackCommand::Play: return transport.nowPlaying.trackId != kInvalidTrackId || transport.ready;
      case PlaybackCommand::Pause: return transport.nowPlaying.trackId != kInvalidTrackId;
      case PlaybackCommand::PlayPause:
        return isActivePlayback(transport.transport) || playIntent(transport) != PlayIntent::None;
      case PlaybackCommand::Next:
      case PlaybackCommand::Previous:
      case PlaybackCommand::Stop:
      case PlaybackCommand::ToggleShuffle:
      case PlaybackCommand::CycleRepeat: return isEnabled(command);
    }

    return false;
  }

  async::Subscription PlaybackActions::onAvailabilityChanged(compat::MoveOnlyFunction<void()> handler)
  {
    return _availabilityChangedSignal.connect(std::move(handler));
  }

  async::Subscription PlaybackActions::onAvailabilityChanged(PlaybackCommand const command,
                                                             compat::MoveOnlyFunction<void()> handler)
  {
    return _commandAvailabilityChangedSignals[commandIndex(command)].connect(std::move(handler));
  }

  void PlaybackActions::handleSnapshot(rt::PlaybackSnapshot const& snapshot)
  {
    auto const& before = _lastSnapshot.transport;
    auto const& beforeSuccession = _lastSnapshot.succession;
    auto const& after = snapshot.transport;
    auto const& afterSuccession = snapshot.succession;

    auto changed = std::array<bool, kCommandCount>{};
    auto const mark = [&changed](std::initializer_list<PlaybackCommand> const commands)
    {
      for (auto const command : commands)
      {
        changed[commandIndex(command)] = true;
      }
    };

    // Map each snapshot field back to the commands whose availability or presented
    // state depends on it, mirroring the granular notifications the dual-service
    // surface used to raise.
    if (after.transport != before.transport)
    {
      mark({PlaybackCommand::Play, PlaybackCommand::Pause, PlaybackCommand::PlayPause, PlaybackCommand::Stop});
    }

    if (after.ready != before.ready)
    {
      mark({PlaybackCommand::Play,
            PlaybackCommand::Pause,
            PlaybackCommand::PlayPause,
            PlaybackCommand::Next,
            PlaybackCommand::Previous,
            PlaybackCommand::ToggleShuffle,
            PlaybackCommand::CycleRepeat});
    }

    if (after.nowPlaying.trackId != before.nowPlaying.trackId)
    {
      mark({PlaybackCommand::Play,
            PlaybackCommand::Pause,
            PlaybackCommand::PlayPause,
            PlaybackCommand::Stop,
            PlaybackCommand::Next,
            PlaybackCommand::Previous});
    }

    if (after.hasDeferredResume != before.hasDeferredResume ||
        after.output.optPendingOutputSelection.has_value() != before.output.optPendingOutputSelection.has_value())
    {
      mark({PlaybackCommand::Play, PlaybackCommand::PlayPause});
    }

    if (afterSuccession.hasNext != beforeSuccession.hasNext ||
        afterSuccession.hasPrevious != beforeSuccession.hasPrevious ||
        afterSuccession.currentTrackId != beforeSuccession.currentTrackId ||
        afterSuccession.sourceState != beforeSuccession.sourceState)
    {
      mark({PlaybackCommand::Next, PlaybackCommand::Previous});
    }

    if (afterSuccession.shuffle != beforeSuccession.shuffle)
    {
      mark({PlaybackCommand::ToggleShuffle});
    }

    if (afterSuccession.repeat != beforeSuccession.repeat)
    {
      mark({PlaybackCommand::CycleRepeat});
    }

    _lastSnapshot = snapshot;

    bool anyChanged = false;

    for (std::size_t index = 0; index < kCommandCount; ++index)
    {
      if (changed[index])
      {
        anyChanged = true;
        _commandAvailabilityChangedSignals[index].emit();
      }
    }

    if (anyChanged)
    {
      _availabilityChangedSignal.emit();
    }
  }
} // namespace ao::uimodel
