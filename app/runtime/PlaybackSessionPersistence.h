// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <chrono>
#include <memory>
#include <stop_token>

namespace ao::rt
{
  class ConfigStore;
  class Library;
  class PlaybackSuccession;
  class PlaybackTransport;
  class PlaybackService;

  /** Executor-affine owner of playback-session serialization, restore, and save policy. */
  class PlaybackSessionPersistence final : public std::enable_shared_from_this<PlaybackSessionPersistence>
  {
  public:
    PlaybackSessionPersistence(ConfigStore& config,
                               Library& library,
                               PlaybackSuccession& succession,
                               PlaybackTransport& playbackTransport,
                               PlaybackService& playback,
                               async::Runtime& asyncRuntime);
    ~PlaybackSessionPersistence();

    PlaybackSessionPersistence(PlaybackSessionPersistence const&) = delete;
    PlaybackSessionPersistence& operator=(PlaybackSessionPersistence const&) = delete;
    PlaybackSessionPersistence(PlaybackSessionPersistence&&) = delete;
    PlaybackSessionPersistence& operator=(PlaybackSessionPersistence&&) = delete;

    Result<> checkpoint();
    Result<> shutdown();
    Result<PlaybackSessionRestoreResult> restore();
    Result<> discardRestorableSession();

  private:
    using Delay = std::chrono::milliseconds;

    static constexpr Delay kSaveDebounceDelay = std::chrono::seconds{1};

    void ensureStarted();
    Result<> save();
    void checkpointBestEffort();
    void handleSnapshot(PlaybackSnapshot const& snapshot);
    void requestDebouncedSave();
    bool hasActiveSession() const;
    bool hasRestorableSession() const;
    void scheduleSave(Delay delay);
    void handleScheduledSave();
    void cancelScheduledSave() noexcept;
    static async::Task<void> waitForScheduledSave(async::Runtime* asyncRuntime,
                                                  std::weak_ptr<PlaybackSessionPersistence> weakSelfPtr,
                                                  Delay delay,
                                                  std::stop_token stopToken);

    ConfigStore& _config;
    Library& _library;
    PlaybackSuccession& _succession;
    PlaybackTransport& _playbackTransport;
    PlaybackService& _playback;
    async::Runtime& _asyncRuntime;
    async::Subscription _successionStateSubscription;
    async::Subscription _snapshotSubscription;
    PlaybackSnapshot _lastSnapshot{};
    async::TaskHandle _scheduledTask;
    bool _sessionDiscarded = false;
    bool _restorePublicationPending = false;
    bool _restoring = false;
    bool _started = false;
    bool _shuttingDown = false;
  };
} // namespace ao::rt
