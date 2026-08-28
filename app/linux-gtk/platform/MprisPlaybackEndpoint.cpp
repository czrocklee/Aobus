// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MprisPlaybackEndpoint.h"

#include "platform/MprisBridge.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ao::gtk::platform
{
  MprisPlaybackEndpoint::MprisPlaybackEndpoint(rt::PlaybackService& playback,
                                               uimodel::PlaybackActions& actions,
                                               MprisBridge::Callbacks& callbacks)
    : _playback{playback}, _playbackCommands{playback.commands()}, _actions{actions}, _callbacks{callbacks}
  {
  }

  bool MprisPlaybackEndpoint::dispatchPlayerMethod(std::string_view const methodName) const
  {
    auto const optCommand = commandForPlayerMethod(methodName);

    if (!optCommand)
    {
      return false;
    }

    _actions.execute(*optCommand);
    return true;
  }

  bool MprisPlaybackEndpoint::dispatchRootMethod(std::string_view const methodName) const
  {
    if (methodName == "Raise")
    {
      return _callbacks.raise && _callbacks.raise();
    }

    if (methodName == "Quit")
    {
      return _callbacks.quit && _callbacks.quit();
    }

    return false;
  }

  bool MprisPlaybackEndpoint::dispatchSeek(std::int64_t const offsetUs)
  {
    auto const& state = _playback.snapshot().transport;

    if (state.nowPlaying.trackId == kInvalidTrackId)
    {
      return true;
    }

    if (isRelativeSeekPastEnd(state, offsetUs))
    {
      _actions.execute(uimodel::PlaybackCommand::Next);
      return true;
    }

    _playbackCommands.seek(MprisBridge::seekTargetElapsed(state, offsetUs));
    return true;
  }

  bool MprisPlaybackEndpoint::dispatchSetPosition(std::string_view const requestedTrackObjectPath,
                                                  std::int64_t const positionUs)
  {
    auto const& state = _playback.snapshot().transport;

    if (state.nowPlaying.trackId == kInvalidTrackId)
    {
      return true;
    }

    if (requestedTrackObjectPath != MprisBridge::trackObjectPath(state.nowPlaying.trackId))
    {
      return true;
    }

    if (positionUs < 0)
    {
      return true;
    }

    auto const elapsed = MprisBridge::fromMprisMicroseconds(positionUs);

    if (state.duration > std::chrono::milliseconds{0} && elapsed > state.duration)
    {
      return true;
    }

    _playbackCommands.seek(elapsed);
    return true;
  }

  bool MprisPlaybackEndpoint::dispatchSetRate(double const rate) const
  {
    if (!std::isfinite(rate))
    {
      return false;
    }

    if (rate == 0.0)
    {
      _actions.execute(uimodel::PlaybackCommand::Pause);
    }

    return true;
  }

  void MprisPlaybackEndpoint::dispatchSetVolume(double const volume)
  {
    _playbackCommands.setVolume(static_cast<float>(volume));
  }

  void MprisPlaybackEndpoint::dispatchSetShuffle(bool const shuffle)
  {
    _playbackCommands.setShuffleMode(shuffle ? rt::ShuffleMode::On : rt::ShuffleMode::Off);
  }

  bool MprisPlaybackEndpoint::dispatchSetLoopStatus(std::string_view const loopStatus)
  {
    auto const optMode = MprisBridge::repeatModeForLoopStatus(loopStatus);

    if (!optMode)
    {
      return false;
    }

    _playbackCommands.setRepeatMode(*optMode);
    return true;
  }

  std::optional<bool> MprisPlaybackEndpoint::playerCapabilityProperty(std::string_view const propertyName) const
  {
    if (propertyName == "CanGoNext")
    {
      return _actions.isCapable(uimodel::PlaybackCommand::Next);
    }

    if (propertyName == "CanGoPrevious")
    {
      return _actions.isCapable(uimodel::PlaybackCommand::Previous);
    }

    if (propertyName == "CanPlay")
    {
      return _actions.isCapable(uimodel::PlaybackCommand::Play);
    }

    if (propertyName == "CanPause")
    {
      return _actions.isCapable(uimodel::PlaybackCommand::Pause);
    }

    if (propertyName == "CanControl")
    {
      return true;
    }

    return std::nullopt;
  }

  bool MprisPlaybackEndpoint::isRelativeSeekPastEnd(rt::PlaybackTransportSnapshot const& state,
                                                    std::int64_t const offsetUs) noexcept
  {
    if (state.duration <= std::chrono::milliseconds{0} || offsetUs <= 0)
    {
      return false;
    }

    auto const offsetMs = MprisBridge::fromMprisMicroseconds(offsetUs).count();

    if (offsetMs <= 0)
    {
      return false;
    }

    auto const elapsedMs = state.elapsed.count();
    auto const durationMs = state.duration.count();

    if (elapsedMs >= durationMs)
    {
      return true;
    }

    return offsetMs > durationMs - elapsedMs;
  }

  std::optional<uimodel::PlaybackCommand> MprisPlaybackEndpoint::commandForPlayerMethod(
    std::string_view const methodName) noexcept
  {
    using Command = uimodel::PlaybackCommand;

    if (methodName == "PlayPause")
    {
      return Command::PlayPause;
    }

    if (methodName == "Play")
    {
      return Command::Play;
    }

    if (methodName == "Pause")
    {
      return Command::Pause;
    }

    if (methodName == "Stop")
    {
      return Command::Stop;
    }

    if (methodName == "Next")
    {
      return Command::Next;
    }

    if (methodName == "Previous")
    {
      return Command::Previous;
    }

    return std::nullopt;
  }
} // namespace ao::gtk::platform
