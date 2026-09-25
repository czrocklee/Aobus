// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "MprisBridge.h"
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

namespace ao::media
{
  class MprisPlaybackEndpoint final
  {
  public:
    MprisPlaybackEndpoint(rt::PlaybackService& playback,
                          uimodel::PlaybackActions& actions,
                          MprisBridge::Callbacks& callbacks);

    bool tryDispatchPlayerMethod(std::string_view methodName) const;
    // Only synchronous Raise is dispatched here; the bridge owns deferred Quit.
    bool tryDispatchRootMethod(std::string_view methodName) const;
    void handleSeek(std::int64_t offsetUs);
    void handleSetPosition(std::string_view requestedTrackObjectPath, std::int64_t positionUs);
    bool tryDispatchSetRate(double rate) const;
    void dispatchSetVolume(double volume);
    void dispatchSetShuffle(bool shuffle);
    bool tryDispatchSetLoopStatus(std::string_view loopStatus);
    std::optional<bool> playerCapabilityProperty(std::string_view propertyName,
                                                 rt::PlaybackTransportSnapshot const& state) const;

  private:
    static std::optional<uimodel::PlaybackCommand> commandForPlayerMethod(std::string_view methodName) noexcept;

    rt::PlaybackService& _playback;
    rt::PlaybackCommands& _playbackCommands;
    uimodel::PlaybackActions& _actions;
    MprisBridge::Callbacks& _callbacks;
  };
} // namespace ao::media
