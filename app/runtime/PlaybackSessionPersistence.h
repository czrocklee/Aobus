// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackSessionRevision.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>

namespace ao::rt
{
  class ConfigStore;
  class Library;
  class PlaybackSequenceService;
  class PlaybackService;

  struct PlaybackSessionPersistenceRestoreResult final
  {
    bool restored = false;
    TrackId trackId = kInvalidTrackId;
    ListId sourceListId = kInvalidListId;
  };

  /** Executor-affine owner of playback-session serialization, restore, and save policy. */
  class PlaybackSessionPersistence final : public std::enable_shared_from_this<PlaybackSessionPersistence>
  {
  public:
    PlaybackSessionPersistence(ConfigStore& config,
                               Library& library,
                               PlaybackSequenceService& sequence,
                               PlaybackService& playback,
                               async::Runtime& asyncRuntime);
    ~PlaybackSessionPersistence();

    PlaybackSessionPersistence(PlaybackSessionPersistence const&) = delete;
    PlaybackSessionPersistence& operator=(PlaybackSessionPersistence const&) = delete;
    PlaybackSessionPersistence(PlaybackSessionPersistence&&) = delete;
    PlaybackSessionPersistence& operator=(PlaybackSessionPersistence&&) = delete;

    void start();
    Result<> checkpoint();
    Result<> shutdown();
    Result<PlaybackSessionPersistenceRestoreResult> restore();
    Result<> discardRestorableSession();
    async::Subscription onDirty(std::move_only_function<void()> handler);

  private:
    using Delay = std::chrono::milliseconds;

    enum class ScheduledSave : std::uint8_t
    {
      None,
      DirtyDebounce,
      Retry,
    };

    static constexpr Delay kDirtyDebounceDelay = std::chrono::seconds{1};
    static constexpr Delay kInitialRetryDelay = std::chrono::seconds{1};
    static constexpr Delay kMaximumRetryDelay = std::chrono::minutes{1};
    static constexpr Delay kPeriodicSaveInterval = std::chrono::seconds{10};

    Result<> save();
    void markDirty();
    bool hasActiveSession() const;
    bool hasRestorableSession() const;
    void handleSaveSucceeded();
    void scheduleRetry();
    void scheduleSave(ScheduledSave kind, Delay delay);
    void handleScheduledSave(std::uint64_t scheduleGeneration, ScheduledSave kind);
    void cancelScheduledSave() noexcept;
    void startPeriodicSave();
    static async::Task<void> waitForScheduledSave(async::Runtime* asyncRuntime,
                                                  std::weak_ptr<PlaybackSessionPersistence> weakSelfPtr,
                                                  Delay delay,
                                                  std::uint64_t scheduleGeneration,
                                                  ScheduledSave kind,
                                                  std::stop_token stopToken);
    static async::Task<void> runPeriodicSave(async::Runtime* asyncRuntime,
                                             std::weak_ptr<PlaybackSessionPersistence> weakSelfPtr,
                                             std::stop_token stopToken);

    ConfigStore& _config;
    Library& _library;
    PlaybackSequenceService& _sequence;
    PlaybackService& _playback;
    async::Runtime& _asyncRuntime;
    async::Signal<> _dirtySignal;
    async::Subscription _sequenceIntentSubscription;
    async::Subscription _volumeSubscription;
    async::Subscription _mutedSubscription;
    async::Subscription _seekSubscription;
    async::Subscription _pausedSubscription;
    async::Subscription _stoppedSubscription;
    async::Subscription _nowPlayingSubscription;
    PlaybackSessionRevision _sessionRevision;
    async::TaskHandle _scheduledTask;
    async::TaskHandle _periodicTask;
    Delay _nextRetryDelay{kInitialRetryDelay};
    std::uint64_t _scheduleGeneration = 0;
    ScheduledSave _scheduledSave = ScheduledSave::None;
    bool _sessionDiscarded = false;
    bool _restoring = false;
    bool _started = false;
    bool _shuttingDown = false;
    std::chrono::milliseconds _intentPosition{0};
    float _volumeIntent = 1.0F;
    bool _mutedIntent = false;
  };
} // namespace ao::rt
