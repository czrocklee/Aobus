// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "platform/MprisBridge.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ao::gtk::platform
{
  class MprisPlaybackEndpoint final
  {
  public:
    MprisPlaybackEndpoint(rt::PlaybackService& playback,
                          uimodel::PlaybackCommandSurface& commands,
                          MprisBridge::Callbacks& callbacks)
      : _playback{playback}, _commands{commands}, _callbacks{callbacks}
    {
    }

    bool dispatchPlayerMethod(std::string_view const methodName) const
    {
      auto const optCommand = commandForPlayerMethod(methodName);

      if (!optCommand)
      {
        return false;
      }

      _commands.execute(*optCommand);
      return true;
    }

    bool dispatchRootMethod(std::string_view const methodName) const
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

    bool dispatchSeek(std::int64_t const offsetUs)
    {
      auto const state = _playback.state();

      if (state.nowPlaying.trackId == kInvalidTrackId)
      {
        return true;
      }

      if (relativeSeekPastEnd(state, offsetUs))
      {
        _commands.execute(uimodel::PlaybackCommand::Next);
        return true;
      }

      _playback.seek(MprisBridge::seekTargetElapsed(state, offsetUs));
      return true;
    }

    bool dispatchSetPosition(std::string_view const requestedTrackObjectPath, std::int64_t const positionUs)
    {
      auto const state = _playback.state();

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

      _playback.seek(elapsed);
      return true;
    }

    bool dispatchSetRate(double const rate) const noexcept { return rate == 1.0; }

    bool dispatchSetVolume(double const volume)
    {
      _playback.setVolume(static_cast<float>(volume));
      return true;
    }

    bool dispatchSetShuffle(bool const shuffle)
    {
      _playback.setShuffleMode(shuffle ? rt::ShuffleMode::On : rt::ShuffleMode::Off);
      return true;
    }

    bool dispatchSetLoopStatus(std::string_view const loopStatus)
    {
      auto const optMode = MprisBridge::repeatModeForLoopStatus(loopStatus);

      if (!optMode)
      {
        return false;
      }

      _playback.setRepeatMode(*optMode);
      return true;
    }

    std::optional<bool> playerCapabilityProperty(std::string_view const propertyName) const
    {
      if (propertyName == "CanGoNext")
      {
        return _commands.capable(uimodel::PlaybackCommand::Next);
      }

      if (propertyName == "CanGoPrevious")
      {
        return _commands.capable(uimodel::PlaybackCommand::Previous);
      }

      if (propertyName == "CanPlay")
      {
        return _commands.capable(uimodel::PlaybackCommand::Play);
      }

      if (propertyName == "CanPause")
      {
        return _commands.capable(uimodel::PlaybackCommand::Pause);
      }

      if (propertyName == "CanControl")
      {
        return true;
      }

      return std::nullopt;
    }

  private:
    static bool relativeSeekPastEnd(rt::PlaybackState const& state, std::int64_t const offsetUs) noexcept
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

    static std::optional<uimodel::PlaybackCommand> commandForPlayerMethod(std::string_view const methodName) noexcept
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

    rt::PlaybackService& _playback;
    uimodel::PlaybackCommandSurface& _commands;
    MprisBridge::Callbacks& _callbacks;
  };
} // namespace ao::gtk::platform
