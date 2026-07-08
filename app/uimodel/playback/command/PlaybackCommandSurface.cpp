// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/Subscription.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/uimodel/playback/queue/PlaybackQueueSession.h>

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    bool hasIdleCurrentTrack(rt::PlaybackState const& state) noexcept
    {
      return state.transport == audio::Transport::Idle && state.nowPlaying.trackId != kInvalidTrackId;
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

    bool canPlay(rt::PlaybackState const& state) noexcept
    {
      return state.ready && (state.transport == audio::Transport::Paused || hasIdleCurrentTrack(state) ||
                             shouldStartSelection(state.transport));
    }

    bool canPlayPause(rt::PlaybackState const& state) noexcept
    {
      return state.ready && (isActivePlayback(state.transport) || state.transport == audio::Transport::Paused ||
                             hasIdleCurrentTrack(state) || shouldStartSelection(state.transport));
    }

    bool canStop(rt::PlaybackState const& state) noexcept
    {
      return isActivePlayback(state.transport) || state.transport == audio::Transport::Paused ||
             state.transport == audio::Transport::Error;
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

  PlaybackCommandSurface::PlaybackCommandSurface(rt::PlaybackService& playback,
                                                 PlaybackQueueSession* queueSession,
                                                 std::function<void()> playSelection)
    : _playback{playback}, _queueSession{queueSession}, _playSelection{std::move(playSelection)}
  {
    subscribeAvailabilityEvents();
  }

  void PlaybackCommandSurface::execute(PlaybackCommand command)
  {
    auto const& state = _playback.state();

    if (!isEnabled(command))
    {
      return;
    }

    switch (command)
    {
      case PlaybackCommand::Play:
        if (state.transport == audio::Transport::Paused || hasIdleCurrentTrack(state))
        {
          _playback.resume();
        }
        else if (shouldStartSelection(state.transport) && _playSelection)
        {
          _playSelection();
        }

        break;

      case PlaybackCommand::Pause: _playback.pause(); break;

      case PlaybackCommand::PlayPause:
        if (isActivePlayback(state.transport))
        {
          _playback.pause();
        }
        else if (state.transport == audio::Transport::Paused || hasIdleCurrentTrack(state))
        {
          _playback.resume();
        }
        else if (shouldStartSelection(state.transport) && _playSelection)
        {
          _playSelection();
        }

        break;

      case PlaybackCommand::Stop: _playback.stop(); break;

      case PlaybackCommand::Next:
        if (_queueSession != nullptr)
        {
          _queueSession->next();
        }

        break;

      case PlaybackCommand::Previous:
        if (_queueSession != nullptr)
        {
          _queueSession->previous();
        }

        break;

      case PlaybackCommand::ToggleShuffle:
        _playback.setShuffleMode(state.mode.shuffle == rt::ShuffleMode::Off ? rt::ShuffleMode::On
                                                                            : rt::ShuffleMode::Off);
        break;

      case PlaybackCommand::CycleRepeat: _playback.setRepeatMode(nextRepeatMode(state.mode.repeat)); break;
    }
  }

  bool PlaybackCommandSurface::isEnabled(PlaybackCommand command) const
  {
    switch (auto const& state = _playback.state(); command)
    {
      case PlaybackCommand::Play: return canPlay(state);
      case PlaybackCommand::Pause: return state.ready && isActivePlayback(state.transport);
      case PlaybackCommand::PlayPause: return canPlayPause(state);
      case PlaybackCommand::Stop: return canStop(state);
      case PlaybackCommand::Next: return state.ready && _queueSession != nullptr && _queueSession->hasNext();
      case PlaybackCommand::Previous: return state.ready && _queueSession != nullptr && _queueSession->hasPrevious();
      case PlaybackCommand::ToggleShuffle:
      case PlaybackCommand::CycleRepeat: return state.ready;
    }

    return false;
  }

  bool PlaybackCommandSurface::isCapable(PlaybackCommand command) const
  {
    switch (auto const& state = _playback.state(); command)
    {
      case PlaybackCommand::Play: return state.nowPlaying.trackId != kInvalidTrackId || state.ready;
      case PlaybackCommand::Pause: return state.nowPlaying.trackId != kInvalidTrackId;
      case PlaybackCommand::Next:
      case PlaybackCommand::Previous:
      case PlaybackCommand::PlayPause:
      case PlaybackCommand::Stop:
      case PlaybackCommand::ToggleShuffle:
      case PlaybackCommand::CycleRepeat: return isEnabled(command);
    }

    return false;
  }

  rt::Subscription PlaybackCommandSurface::onAvailabilityChanged(std::move_only_function<void()> handler)
  {
    return _availabilityChangedSignal.connect(std::move(handler));
  }

  rt::Subscription PlaybackCommandSurface::onAvailabilityChanged(PlaybackCommand const command,
                                                                 std::move_only_function<void()> handler)
  {
    return _commandAvailabilityChangedSignals[commandIndex(command)].connect(std::move(handler));
  }

  void PlaybackCommandSurface::subscribeAvailabilityEvents()
  {
    auto const notifyTransport = [this]
    {
      emitAvailabilityChanged(
        {PlaybackCommand::Play, PlaybackCommand::Pause, PlaybackCommand::PlayPause, PlaybackCommand::Stop});
    };
    auto const notifyReady = [this]
    {
      emitAvailabilityChanged({PlaybackCommand::Play,
                               PlaybackCommand::Pause,
                               PlaybackCommand::PlayPause,
                               PlaybackCommand::ToggleShuffle,
                               PlaybackCommand::CycleRepeat});
    };

    _availabilitySubs.push_back(_playback.onPreparing(notifyTransport));
    _availabilitySubs.push_back(_playback.onStarted(notifyTransport));
    _availabilitySubs.push_back(_playback.onPaused(notifyTransport));
    _availabilitySubs.push_back(_playback.onIdle(notifyTransport));
    _availabilitySubs.push_back(_playback.onStopped(notifyTransport));
    _availabilitySubs.push_back(_playback.onNowPlayingChanged(
      [this](rt::PlaybackService::NowPlayingChanged const&)
      {
        emitAvailabilityChanged({PlaybackCommand::Play,
                                 PlaybackCommand::Pause,
                                 PlaybackCommand::PlayPause,
                                 PlaybackCommand::Stop,
                                 PlaybackCommand::Next,
                                 PlaybackCommand::Previous});
      }));
    _availabilitySubs.push_back(_playback.onShuffleModeChanged(
      [this](rt::PlaybackService::ShuffleModeChanged const&)
      { emitAvailabilityChanged({PlaybackCommand::ToggleShuffle, PlaybackCommand::Next}); }));
    _availabilitySubs.push_back(_playback.onRepeatModeChanged(
      [this](rt::PlaybackService::RepeatModeChanged const&)
      { emitAvailabilityChanged({PlaybackCommand::CycleRepeat, PlaybackCommand::Next, PlaybackCommand::Previous}); }));
    _availabilitySubs.push_back(_playback.onSeekUpdate(
      [this](rt::PlaybackService::SeekUpdate const& event)
      {
        if (event.mode == rt::PlaybackService::SeekMode::Final)
        {
          emitAvailabilityChanged({PlaybackCommand::Previous});
        }
      }));
    _availabilitySubs.push_back(
      _playback.onOutputDeviceChanged([notifyReady](rt::OutputDeviceSelection const&) { notifyReady(); }));
    _availabilitySubs.push_back(_playback.onOutputDevicesChanged(notifyReady));
    _availabilitySubs.push_back(
      _playback.onQualityChanged([notifyReady](rt::PlaybackService::QualityChanged const&) { notifyReady(); }));
  }

  void PlaybackCommandSurface::emitAvailabilityChanged(std::initializer_list<PlaybackCommand> const commands)
  {
    _availabilityChangedSignal.emit();

    for (auto const command : commands)
    {
      _commandAvailabilityChangedSignals[commandIndex(command)].emit();
    }
  }
} // namespace ao::uimodel
