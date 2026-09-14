// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace ao::rt
{
  class PlaybackService;
  class ResourceByteMemoryCache;
}

namespace ao::uimodel
{
  class PlaybackActions;
}

namespace ao::appkit
{
  enum class MediaRemoteCommand : std::uint8_t
  {
    Play,
    Pause,
    PlayPause,
    Previous,
    Next,
    Stop,
    ChangePosition,
  };

  enum class MediaRemoteCommandStatus : std::uint8_t
  {
    Success,
    NoActionableNowPlayingItem,
    CommandFailed,
  };

  struct MediaRemoteCommandEvent final
  {
    MediaRemoteCommand command = MediaRemoteCommand::PlayPause;
    std::optional<double> optPositionSeconds = std::nullopt;
  };

  using MediaRemoteCommandHandler = std::function<MediaRemoteCommandStatus(MediaRemoteCommandEvent const&)>;

  struct MediaPlayerDiagnostics final
  {
    std::size_t registeredCommandCount = 0;
    std::uint64_t admittedCommandCount = 0;
    std::uint64_t settledCommandCount = 0;
    // Successful adapter handling, including idempotent no-ops and runtime
    // command submissions; not eventual audio completion.
    std::uint64_t handledCommandCount = 0;
    bool retired = false;
  };

  /** Session-lifetime macOS Now Playing and remote-command adapter. */
  class [[nodiscard]] MediaPlayerAdapter final
  {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    MediaPlayerAdapter(rt::PlaybackService& playback,
                       uimodel::PlaybackActions& actions,
                       rt::ResourceByteMemoryCache& resourceBytes,
                       Clock clock = std::chrono::steady_clock::now);
    ~MediaPlayerAdapter();

    MediaPlayerAdapter(MediaPlayerAdapter const&) = delete;
    MediaPlayerAdapter& operator=(MediaPlayerAdapter const&) = delete;
    MediaPlayerAdapter(MediaPlayerAdapter&&) = delete;
    MediaPlayerAdapter& operator=(MediaPlayerAdapter&&) = delete;

    /** An independently owned callback sharing the native admission lifetime. */
    MediaRemoteCommandHandler remoteCommandHandler() const;

    bool isCommandAvailable(MediaRemoteCommand command) const noexcept;
    MediaPlayerDiagnostics diagnostics() const noexcept;
    bool isPerforming() const noexcept;
    void retire() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::appkit
