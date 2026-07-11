// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/Subscription.h>

#include <chrono>
#include <functional>
#include <memory>

namespace ao::rt
{
  using std::chrono_literals::operator""min;
  using std::chrono_literals::operator""s;

  /**
   * UI-neutral timing owner for playback-session persistence.
   *
   * Runtime owns dirty authority and payload construction. This service
   * owns only dirty debounce, retry timing, and frontend save triggers.
   */
  class PlaybackSessionSaveService final
  {
  public:
    using Delay = std::chrono::milliseconds;
    using Callback = std::move_only_function<void()>;
    using DirtySubscriber = std::move_only_function<Subscription(Callback)>;
    using SaveOperation = std::move_only_function<Result<>()>;

    static constexpr Delay kDefaultDirtyDebounceDelay = 1s;
    static constexpr Delay kDefaultInitialRetryDelay = 1s;
    static constexpr Delay kDefaultMaximumRetryDelay = 1min;

    struct Port final
    {
      DirtySubscriber subscribeDirty;
      SaveOperation save;
    };

    struct Timing final
    {
      Delay dirtyDebounceDelay = kDefaultDirtyDebounceDelay;
      Delay initialRetryDelay = kDefaultInitialRetryDelay;
      Delay maximumRetryDelay = kDefaultMaximumRetryDelay;
    };

    class Scheduler
    {
    public:
      virtual ~Scheduler() = default;

      Scheduler(Scheduler const&) = delete;
      Scheduler& operator=(Scheduler const&) = delete;
      Scheduler(Scheduler&&) = delete;
      Scheduler& operator=(Scheduler&&) = delete;

      /**
       * Schedules one deferred callback. Resetting the returned registration
       * cancels delivery. The callback is never invoked before schedule returns.
       */
      virtual Subscription schedule(Delay delay, Callback callback) = 0;

    protected:
      Scheduler() = default;
    };

    PlaybackSessionSaveService(Port port, Scheduler& scheduler);
    PlaybackSessionSaveService(Port port, Scheduler& scheduler, Timing timing);
    ~PlaybackSessionSaveService();

    PlaybackSessionSaveService(PlaybackSessionSaveService const&) = delete;
    PlaybackSessionSaveService& operator=(PlaybackSessionSaveService const&) = delete;
    PlaybackSessionSaveService(PlaybackSessionSaveService&&) = delete;
    PlaybackSessionSaveService& operator=(PlaybackSessionSaveService&&) = delete;

    /** Starts dirty observation. Safe to call more than once. */
    void start();

    /** Performs an immediate significant-event save through normal retry policy. */
    void saveSignificantEvent();

    /** Performs an immediate periodic safety-net save through normal retry policy. */
    void savePeriodic();

    /** Cancels deferred work and makes one final immediate save attempt. */
    Result<> shutdown();

  private:
    struct Impl;
    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
