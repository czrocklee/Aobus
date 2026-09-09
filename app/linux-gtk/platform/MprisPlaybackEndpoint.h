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
  class PlaybackActions;
}

namespace ao::gtk::platform
{
  class MprisPlaybackEndpoint final
  {
  public:
    MprisPlaybackEndpoint(rt::PlaybackService& playback,
                          uimodel::PlaybackActions& actions,
                          MprisBridge::Callbacks& callbacks);

    bool tryDispatchPlayerMethod(std::string_view methodName) const;
    bool tryDispatchRootMethod(std::string_view methodName) const;
    bool tryHandleSeek(std::int64_t offsetUs);
    bool tryHandleSetPosition(std::string_view requestedTrackObjectPath, std::int64_t positionUs);
    bool tryDispatchSetRate(double rate) const;
    void dispatchSetVolume(double volume);
    void dispatchSetShuffle(bool shuffle);
    bool tryDispatchSetLoopStatus(std::string_view loopStatus);
    std::optional<bool> playerCapabilityProperty(std::string_view propertyName) const;

  private:
    static bool isRelativeSeekPastEnd(rt::PlaybackTransportSnapshot const& state, std::int64_t offsetUs) noexcept;
    static std::optional<uimodel::PlaybackCommand> commandForPlayerMethod(std::string_view methodName) noexcept;

    rt::PlaybackService& _playback;
    rt::PlaybackCommands& _playbackCommands;
    uimodel::PlaybackActions& _actions;
    MprisBridge::Callbacks& _callbacks;
  };
} // namespace ao::gtk::platform
