// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/utility/ScopedRegistration.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ao::async
{
  class Executor;
}

namespace ao::rt
{
  class PlaybackService;
}

namespace Glib
{
  class VariantBase;
}

namespace ao::uimodel
{
  class PlaybackActions;
}

namespace ao::media
{
  struct MprisBridgeOptions final
  {
    std::string busName = "org.mpris.MediaPlayer2.aobus";
    std::string identity = "Aobus";
    // Empty omits the optional DesktopEntry property.
    std::string desktopEntry = "aobus";
    bool uniqueInstance = false;
    // Absent uses the session-bus environment, or an existing XDG runtime bus
    // only when that environment variable is unset. Explicit empty disables it.
    std::optional<std::string> optBusAddress{};
  };

  // Owner-executor confined. Borrowed collaborators outlive destruction, which
  // joins the native producer. Only the deferred Quit callback may destroy the
  // bridge; other callbacks must not destroy it reentrantly.
  class [[nodiscard]] MprisBridge final
  {
  public:
    using RootCommand = std::function<bool()>;
    using OnArtUrlReady = std::function<void(std::string)>;
    using ArtUrlRequester = std::function<utility::ScopedRegistration(ResourceId, OnArtUrlReady)>;

    struct PlaybackSource final
    {
      std::function<rt::PlaybackSnapshot const&()> snapshot;
      std::function<async::Subscription(rt::PlaybackSnapshotObserver)> onSnapshot;
      std::function<std::chrono::milliseconds()> elapsed;
    };

    struct Callbacks final
    {
      RootCommand raise{};
      // Presence admits a quit attempt; the bridge defers it until after reply
      // submission. The host still owns save waiting and normal exit policy.
      std::function<void()> quit{};
      ArtUrlRequester requestArtUrl{};
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

    MprisBridge(async::Executor& executor,
                rt::PlaybackService& playback,
                uimodel::PlaybackActions& actions,
                Callbacks callbacks,
                MprisBridgeOptions options = {});
    MprisBridge(async::Executor& executor,
                rt::PlaybackService& playback,
                uimodel::PlaybackActions& actions,
                Callbacks callbacks,
                PlaybackSource playbackSource,
                MprisBridgeOptions options = {});
    ~MprisBridge();

    MprisBridge(MprisBridge const&) = delete;
    MprisBridge& operator=(MprisBridge const&) = delete;
    MprisBridge(MprisBridge&&) = delete;
    MprisBridge& operator=(MprisBridge&&) = delete;

    void start();
    /// Closes command/artwork admission immediately without waiting for the bus.
    void retire();

    bool isActive() const noexcept;
    /// Acquired bus name, empty before activation or after retirement.
    std::string_view busName() const noexcept;
    MetadataSnapshot metadataSnapshot() const;
    /** Reads the same Player property mapping exported over D-Bus. */
    Glib::VariantBase playerProperty(std::string_view propertyName) const;

    static std::string_view playbackStatus(audio::Transport transport) noexcept;
    static std::string_view loopStatus(rt::RepeatMode mode) noexcept;
    static std::optional<rt::RepeatMode> repeatModeForLoopStatus(std::string_view loopStatus) noexcept;
    static std::int64_t microsecondsFromMilliseconds(std::chrono::milliseconds duration) noexcept;
    static std::chrono::milliseconds fromMprisMicroseconds(std::int64_t value) noexcept;
    static bool shouldEmitSeeked(rt::PlaybackTransportSnapshot const& before,
                                 rt::PlaybackTransportSnapshot const& after) noexcept;
    static bool shouldEmitMetadataChanged(rt::PlaybackTransportSnapshot const& before,
                                          rt::PlaybackTransportSnapshot const& after) noexcept;
    static std::string trackObjectPath(TrackId trackId, rt::PlaybackOccurrenceId occurrenceId);
    static MetadataSnapshot metadataForState(rt::PlaybackTransportSnapshot const& state, std::string artUrl = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::media
