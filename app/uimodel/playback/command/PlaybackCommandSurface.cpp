// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <array>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    bool hasIdleSequenceTrack(rt::PlaybackTransportSnapshot const& transport,
                              rt::PlaybackSuccessionSnapshot const& succession) noexcept
    {
      return transport.transport == audio::Transport::Idle && transport.nowPlaying.trackId != kInvalidTrackId &&
             succession.currentTrackId == transport.nowPlaying.trackId;
    }

    bool isActivePlayback(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Opening || transport == audio::Transport::Buffering ||
             transport == audio::Transport::Playing || transport == audio::Transport::Seeking;
    }

    bool shouldStartSelection(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Idle || transport == audio::Transport::Error;
    }

    bool canPlay(rt::PlaybackTransportSnapshot const& transport,
                 rt::PlaybackSuccessionSnapshot const& succession) noexcept
    {
      if (transport.transport == audio::Transport::Paused)
      {
        return true;
      }

      return transport.ready &&
             (hasIdleSequenceTrack(transport, succession) || shouldStartSelection(transport.transport));
    }

    bool canPlayPause(rt::PlaybackTransportSnapshot const& transport,
                      rt::PlaybackSuccessionSnapshot const& succession) noexcept
    {
      if (isActivePlayback(transport.transport) || transport.transport == audio::Transport::Paused)
      {
        return true;
      }

      return transport.ready &&
             (hasIdleSequenceTrack(transport, succession) || shouldStartSelection(transport.transport));
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

  PlaybackCommandSurface::PlaybackCommandSurface(rt::PlaybackService& playback, std::function<void()> playSelection)
    : _playback{playback}
    , _commands{playback.commands()}
    , _playSelection{std::move(playSelection)}
    , _lastSnapshot{playback.snapshot()}
  {
    _snapshotSub =
      _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { handleSnapshot(snapshot); });
  }

  bool PlaybackCommandSurface::execute(PlaybackCommand command)
  {
    if (!isEnabled(command))
    {
      return false;
    }

    auto const& snapshot = _playback.snapshot();
    auto const& transport = snapshot.transport;
    auto const& succession = snapshot.succession;
    auto const resumeCurrent =
      transport.transport == audio::Transport::Paused || hasIdleSequenceTrack(transport, succession);

    switch (command)
    {
      case PlaybackCommand::Play:
        if (resumeCurrent)
        {
          _commands.resume();
        }
        else if (shouldStartSelection(transport.transport) && _playSelection)
        {
          _playSelection();
        }

        break;

      case PlaybackCommand::Pause: _commands.pause(); break;

      case PlaybackCommand::PlayPause:
        if (isActivePlayback(transport.transport))
        {
          _commands.pause();
        }
        else if (resumeCurrent)
        {
          _commands.resume();
        }
        else if (shouldStartSelection(transport.transport) && _playSelection)
        {
          _playSelection();
        }

        break;

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

  bool PlaybackCommandSurface::isEnabled(PlaybackCommand command) const
  {
    auto const& snapshot = _playback.snapshot();
    auto const& transport = snapshot.transport;

    switch (auto const& succession = snapshot.succession; command)
    {
      case PlaybackCommand::Play: return canPlay(transport, succession);
      case PlaybackCommand::Pause: return isActivePlayback(transport.transport);
      case PlaybackCommand::PlayPause: return canPlayPause(transport, succession);
      case PlaybackCommand::Stop: return canStop(transport);
      case PlaybackCommand::Next: return transport.ready && succession.hasNext;
      case PlaybackCommand::Previous: return transport.ready && succession.hasPrevious;
      case PlaybackCommand::ToggleShuffle:
      case PlaybackCommand::CycleRepeat: return transport.ready;
    }

    return false;
  }

  bool PlaybackCommandSurface::isCapable(PlaybackCommand command) const
  {
    switch (auto const& transport = _playback.snapshot().transport; command)
    {
      case PlaybackCommand::Play: return transport.nowPlaying.trackId != kInvalidTrackId || transport.ready;
      case PlaybackCommand::Pause: return transport.nowPlaying.trackId != kInvalidTrackId;
      case PlaybackCommand::Next:
      case PlaybackCommand::Previous:
      case PlaybackCommand::PlayPause:
      case PlaybackCommand::Stop:
      case PlaybackCommand::ToggleShuffle:
      case PlaybackCommand::CycleRepeat: return isEnabled(command);
    }

    return false;
  }

  async::Subscription PlaybackCommandSurface::onAvailabilityChanged(std::move_only_function<void()> handler)
  {
    return _availabilityChangedSignal.connect(std::move(handler));
  }

  async::Subscription PlaybackCommandSurface::onAvailabilityChanged(PlaybackCommand const command,
                                                                    std::move_only_function<void()> handler)
  {
    return _commandAvailabilityChangedSignals[commandIndex(command)].connect(std::move(handler));
  }

  void PlaybackCommandSurface::handleSnapshot(rt::PlaybackSnapshot const& snapshot)
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
