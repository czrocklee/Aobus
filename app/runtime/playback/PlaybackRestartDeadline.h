// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/compat/MoveOnlyFunction.h>

#include <chrono>
#include <memory>

namespace ao::async
{
  class Runtime;
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
    using LiveElapsedReader = compat::MoveOnlyFunction<Elapsed()>;
    using AvailabilityChangedHandler = compat::MoveOnlyFunction<void(bool)>;

    static constexpr Elapsed kRestartThreshold{3000};
    static constexpr Elapsed kFirstRestartAvailableElapsed{3001};

    PlaybackRestartDeadline(async::Runtime& asyncRuntime,
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
    void replaceSession(Elapsed elapsed, bool playing);
    void clearSession() noexcept;
    void shutdown() noexcept;

    bool isActive() const noexcept;
    bool isRunning() const noexcept;
    bool restartAvailable() const noexcept;
    bool hasScheduledDeadline() const noexcept;

  private:
    struct State;

    std::unique_ptr<State> _statePtr;
  };
} // namespace ao::rt
