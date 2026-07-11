// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace ao::async
{
  class Executor;
}

namespace ao::rt
{
  /**
   * Executor-affine previous-restart threshold scheduler.
   *
   * Control methods are called on the runtime executor. Scheduler callbacks may
   * arrive on another thread and are always marshalled back before live elapsed
   * state is read or availability is published.
   */
  class PlaybackRestartDeadline final
  {
  public:
    using Elapsed = std::chrono::milliseconds;
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using DeadlineCallback = std::move_only_function<void()>;
    using MonotonicClock = std::move_only_function<TimePoint()>;
    using LiveElapsedReader = std::move_only_function<Elapsed()>;
    using AvailabilityChangedHandler = std::move_only_function<void(bool)>;

    static constexpr Elapsed kRestartThreshold{3000};
    static constexpr Elapsed kFirstRestartAvailableElapsed{3001};

    class Scheduler
    {
    public:
      virtual ~Scheduler() = default;

      Scheduler(Scheduler const&) = delete;
      Scheduler& operator=(Scheduler const&) = delete;
      Scheduler(Scheduler&&) = delete;
      Scheduler& operator=(Scheduler&&) = delete;

      virtual void schedule(TimePoint deadline, DeadlineCallback callback) = 0;
      virtual void cancel() noexcept = 0;

    protected:
      Scheduler() = default;
    };

    PlaybackRestartDeadline(async::Executor& executor,
                            Scheduler& scheduler,
                            MonotonicClock monotonicClock,
                            LiveElapsedReader liveElapsedReader,
                            AvailabilityChangedHandler availabilityChangedHandler);
    ~PlaybackRestartDeadline();

    PlaybackRestartDeadline(PlaybackRestartDeadline const&) = delete;
    PlaybackRestartDeadline& operator=(PlaybackRestartDeadline const&) = delete;
    PlaybackRestartDeadline(PlaybackRestartDeadline&&) = delete;
    PlaybackRestartDeadline& operator=(PlaybackRestartDeadline&&) = delete;

    void start(Elapsed elapsed);
    void resume(Elapsed elapsed);
    void pause(Elapsed elapsed);
    void seek(Elapsed elapsed);
    void currentTrackChanged(Elapsed elapsed, bool playing);
    void replaceSession(Elapsed elapsed, bool playing);
    void clearSession() noexcept;
    void shutdown() noexcept;

    bool isActive() const noexcept;
    bool isRunning() const noexcept;
    bool restartAvailable() const noexcept;
    bool hasScheduledDeadline() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
