// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/layout/action/LayoutActionTypes.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk::platform
{
  class MprisBridge final
  {
  public:
    using ActionActivator = std::function<uimodel::LayoutActionActivationOutcome(std::string_view)>;
    using ActionAvailabilityResolver = std::function<uimodel::LayoutActionAvailability(std::string_view)>;
    using RootCommand = std::function<bool()>;
    using ArtUrlResolver = std::function<std::string(ResourceId)>;

    struct Callbacks final
    {
      ActionActivator activateAction{};
      ActionAvailabilityResolver actionAvailability{};
      RootCommand raise{};
      RootCommand quit{};
      ArtUrlResolver artUrlForResource{};
    };

    struct MetadataSnapshot final
    {
      std::string trackObjectPath{};
      std::string title{};
      std::string artist{};
      std::string album{};
      std::string artUrl{};
      std::int64_t lengthUs = 0;
    };

    MprisBridge(rt::PlaybackService& playback, Callbacks callbacks);
    ~MprisBridge();

    MprisBridge(MprisBridge const&) = delete;
    MprisBridge& operator=(MprisBridge const&) = delete;
    MprisBridge(MprisBridge&&) = delete;
    MprisBridge& operator=(MprisBridge&&) = delete;

    void start();
    bool dispatchRootMethod(std::string_view methodName);
    bool dispatchPlayerMethod(std::string_view methodName);
    bool dispatchSeek(std::int64_t offsetUs);
    bool dispatchSetPosition(std::string_view trackObjectPath, std::int64_t positionUs);
    bool dispatchSetVolume(double volume);
    bool dispatchSetShuffle(bool shuffle);
    bool dispatchSetLoopStatus(std::string_view loopStatus);

    bool active() const noexcept;

    static std::string_view playbackStatus(audio::Transport transport) noexcept;
    static std::string_view loopStatus(rt::RepeatMode mode) noexcept;
    static std::optional<rt::RepeatMode> repeatModeForLoopStatus(std::string_view loopStatus) noexcept;
    static std::optional<std::string_view> actionForPlayerMethod(std::string_view methodName) noexcept;
    static std::int64_t microsecondsFromMilliseconds(std::chrono::milliseconds duration) noexcept;
    static std::chrono::milliseconds fromMprisMicroseconds(std::int64_t value) noexcept;
    static std::chrono::milliseconds clampElapsed(rt::PlaybackState const& state,
                                                  std::chrono::milliseconds elapsed) noexcept;
    static std::chrono::milliseconds seekTargetElapsed(rt::PlaybackState const& state, std::int64_t offsetUs) noexcept;
    static std::string trackObjectPath(TrackId trackId);
    static MetadataSnapshot metadataForState(rt::PlaybackState const& state, std::string artUrl = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::gtk::platform
