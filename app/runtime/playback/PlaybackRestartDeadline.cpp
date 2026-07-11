// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackRestartDeadline.h"

#include <ao/Exception.h>
#include <ao/async/Executor.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

namespace ao::rt
{
  struct PlaybackRestartDeadline::Impl final : std::enable_shared_from_this<Impl>
  {
    Impl(ao::async::Executor& executor,
         Scheduler& scheduler,
         MonotonicClock monotonicClock,
         LiveElapsedReader liveElapsedReader,
         AvailabilityChangedHandler availabilityChangedHandler)
      : executor{executor}
      , scheduler{scheduler}
      , monotonicClock{std::move(monotonicClock)}
      , liveElapsedReader{std::move(liveElapsedReader)}
      , availabilityChangedHandler{std::move(availabilityChangedHandler)}
    {
      if (!this->monotonicClock)
      {
        throwException<Exception>("Playback restart deadline requires a monotonic clock");
      }

      if (!this->liveElapsedReader)
      {
        throwException<Exception>("Playback restart deadline requires a live elapsed reader");
      }

      if (!this->availabilityChangedHandler)
      {
        throwException<Exception>("Playback restart deadline requires an availability handler");
      }
    }

    static Elapsed normalizeElapsed(Elapsed const elapsed) noexcept { return std::max(elapsed, Elapsed::zero()); }

    void start(Elapsed const elapsed)
    {
      if (shuttingDown)
      {
        return;
      }

      active = true;
      running = true;
      synchronize(normalizeElapsed(elapsed));
    }

    void resume(Elapsed const elapsed)
    {
      if (shuttingDown || !active)
      {
        return;
      }

      running = true;
      synchronize(normalizeElapsed(elapsed));
    }

    void pause(Elapsed const elapsed)
    {
      if (shuttingDown || !active)
      {
        return;
      }

      running = false;
      synchronize(normalizeElapsed(elapsed));
    }

    void seek(Elapsed const elapsed)
    {
      if (shuttingDown || !active)
      {
        return;
      }

      synchronize(normalizeElapsed(elapsed));
    }

    void resetCurrent(Elapsed const elapsed, bool const playing)
    {
      if (shuttingDown)
      {
        return;
      }

      active = true;
      running = playing;
      synchronize(normalizeElapsed(elapsed));
    }

    void clearSession() noexcept
    {
      if (shuttingDown)
      {
        return;
      }

      cancelDeadline();
      active = false;
      running = false;
      restartAvailable = false;
    }

    void shutdown() noexcept
    {
      if (shuttingDown)
      {
        return;
      }

      shuttingDown = true;
      cancelDeadline();
      active = false;
      running = false;
      restartAvailable = false;
    }

    void synchronize(Elapsed const elapsed)
    {
      cancelDeadline();
      auto const synchronizeGeneration = generation;
      setRestartAvailable(elapsed > kRestartThreshold);

      if (generation != synchronizeGeneration || shuttingDown || !active || !running || restartAvailable)
      {
        return;
      }

      scheduleDeadline(elapsed, synchronizeGeneration);
    }

    void scheduleDeadline(Elapsed const elapsed, std::uint64_t const scheduleGeneration)
    {
      auto const delay = kFirstRestartAvailableElapsed - elapsed;
      auto const deadline = monotonicClock() + std::chrono::duration_cast<Clock::duration>(delay);
      auto const weakSelfPtr = weak_from_this();
      deadlineScheduled = true;

      try
      {
        scheduler.schedule(deadline,
                           [weakSelfPtr, scheduleGeneration]
                           {
                             if (auto const selfPtr = weakSelfPtr.lock(); selfPtr != nullptr)
                             {
                               selfPtr->executor.dispatch(
                                 [weakSelfPtr, scheduleGeneration]
                                 {
                                   if (auto const executorSelfPtr = weakSelfPtr.lock(); executorSelfPtr != nullptr)
                                   {
                                     executorSelfPtr->handleDeadline(scheduleGeneration);
                                   }
                                 });
                             }
                           });
      }
      catch (...)
      {
        deadlineScheduled = false;
        throw;
      }
    }

    void handleDeadline(std::uint64_t const scheduleGeneration)
    {
      if (shuttingDown || scheduleGeneration != generation || !active || !running || !deadlineScheduled)
      {
        return;
      }

      deadlineScheduled = false;
      auto const liveElapsed = normalizeElapsed(liveElapsedReader());

      if (liveElapsed > kRestartThreshold)
      {
        setRestartAvailable(true);
        return;
      }

      synchronize(liveElapsed);
    }

    void cancelDeadline() noexcept
    {
      ++generation;

      if (deadlineScheduled)
      {
        scheduler.cancel();
        deadlineScheduled = false;
      }
    }

    void setRestartAvailable(bool const available)
    {
      if (restartAvailable == available)
      {
        return;
      }

      restartAvailable = available;
      availabilityChangedHandler(available);
    }

    ao::async::Executor& executor;
    Scheduler& scheduler;
    MonotonicClock monotonicClock;
    LiveElapsedReader liveElapsedReader;
    AvailabilityChangedHandler availabilityChangedHandler;
    std::uint64_t generation = 0;
    bool active = false;
    bool running = false;
    bool restartAvailable = false;
    bool deadlineScheduled = false;
    bool shuttingDown = false;
  };

  PlaybackRestartDeadline::PlaybackRestartDeadline(ao::async::Executor& executor,
                                                   Scheduler& scheduler,
                                                   MonotonicClock monotonicClock,
                                                   LiveElapsedReader liveElapsedReader,
                                                   AvailabilityChangedHandler availabilityChangedHandler)
    : _implPtr{std::make_shared<Impl>(executor,
                                      scheduler,
                                      std::move(monotonicClock),
                                      std::move(liveElapsedReader),
                                      std::move(availabilityChangedHandler))}
  {
  }

  PlaybackRestartDeadline::~PlaybackRestartDeadline()
  {
    shutdown();
  }

  void PlaybackRestartDeadline::start(Elapsed const elapsed)
  {
    auto const implPtr = _implPtr;
    implPtr->start(elapsed);
  }

  void PlaybackRestartDeadline::resume(Elapsed const elapsed)
  {
    auto const implPtr = _implPtr;
    implPtr->resume(elapsed);
  }

  void PlaybackRestartDeadline::pause(Elapsed const elapsed)
  {
    auto const implPtr = _implPtr;
    implPtr->pause(elapsed);
  }

  void PlaybackRestartDeadline::seek(Elapsed const elapsed)
  {
    auto const implPtr = _implPtr;
    implPtr->seek(elapsed);
  }

  void PlaybackRestartDeadline::currentTrackChanged(Elapsed const elapsed, bool const playing)
  {
    auto const implPtr = _implPtr;
    implPtr->resetCurrent(elapsed, playing);
  }

  void PlaybackRestartDeadline::replaceSession(Elapsed const elapsed, bool const playing)
  {
    auto const implPtr = _implPtr;
    implPtr->resetCurrent(elapsed, playing);
  }

  void PlaybackRestartDeadline::clearSession() noexcept
  {
    auto const implPtr = _implPtr;
    implPtr->clearSession();
  }

  void PlaybackRestartDeadline::shutdown() noexcept
  {
    auto const implPtr = _implPtr;
    implPtr->shutdown();
  }

  bool PlaybackRestartDeadline::isActive() const noexcept
  {
    return _implPtr->active;
  }

  bool PlaybackRestartDeadline::isRunning() const noexcept
  {
    return _implPtr->running;
  }

  bool PlaybackRestartDeadline::restartAvailable() const noexcept
  {
    return _implPtr->restartAvailable;
  }

  bool PlaybackRestartDeadline::hasScheduledDeadline() const noexcept
  {
    return _implPtr->deadlineScheduled;
  }
} // namespace ao::rt
