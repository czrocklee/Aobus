// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "platform/MprisBridge.h"
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace ao::rt
{
  class PlaybackCommands;
  class PlaybackService;
}

namespace ao::uimodel
{
  enum class PlaybackCommand : std::uint8_t;
  class PlaybackCommandSurface;
}

namespace ao::gtk::platform
{
  class MprisPlaybackEndpoint final
  {
  public:
    MprisPlaybackEndpoint(rt::PlaybackService& playback,
                          uimodel::PlaybackCommandSurface& commands,
                          MprisBridge::Callbacks& callbacks);

    bool dispatchPlayerMethod(std::string_view methodName) const;
    bool dispatchRootMethod(std::string_view methodName) const;
    bool dispatchSeek(std::int64_t offsetUs);
    bool dispatchSetPosition(std::string_view requestedTrackObjectPath, std::int64_t positionUs);
    bool dispatchSetRate(double rate) const;
    bool dispatchSetVolume(double volume);
    bool dispatchSetShuffle(bool shuffle);
    bool dispatchSetLoopStatus(std::string_view loopStatus);
    std::optional<bool> playerCapabilityProperty(std::string_view propertyName) const;

  private:
    static bool isRelativeSeekPastEnd(rt::PlaybackTransportSnapshot const& state, std::int64_t offsetUs) noexcept;
    static std::optional<uimodel::PlaybackCommand> commandForPlayerMethod(std::string_view methodName) noexcept;

    rt::PlaybackService& _playback;
    rt::PlaybackCommands& _playbackCommands;
    uimodel::PlaybackCommandSurface& _commands;
    MprisBridge::Callbacks& _callbacks;
  };
} // namespace ao::gtk::platform
