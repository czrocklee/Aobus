// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackRestartDeadline.h"

#include <ao/Exception.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <utility>

namespace ao::rt
{
  struct PlaybackRestartDeadline::SharedState final : std::enable_shared_from_this<SharedState>
  {
    SharedState(async::Runtime& asyncRuntime,
                LiveElapsedReader liveElapsedReader,
                AvailabilityChangedHandler availabilityChangedHandler)
      : asyncRuntime{asyncRuntime}
      , liveElapsedReader{std::move(liveElapsedReader)}
      , availabilityChangedHandler{std::move(availabilityChangedHandler)}
    {
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
      auto const synchronizeRevision = synchronizationRevision;
      setRestartAvailable(elapsed > kRestartThreshold);

      if (synchronizationRevision != synchronizeRevision || shuttingDown || !active || !running || restartAvailable)
      {
        return;
      }

      scheduleDeadline(elapsed);
    }

    void scheduleDeadline(Elapsed const elapsed)
    {
      auto const delay = kFirstRestartAvailableElapsed - elapsed;
      auto const weakStatePtr = weak_from_this();
      deadlineScheduled = true;

      try
      {
        deadlineTask = asyncRuntime.spawnCancellable(
          [asyncRuntime = &asyncRuntime, weakStatePtr, delay](std::stop_token const stopToken)
          { return waitForDeadline(asyncRuntime, weakStatePtr, delay, stopToken); });
      }
      catch (...)
      {
        deadlineScheduled = false;
        throw;
      }
    }

    static async::Task<void> waitForDeadline(async::Runtime* asyncRuntime,
                                             std::weak_ptr<SharedState> weakStatePtr,
                                             Elapsed const delay,
                                             std::stop_token const stopToken)
    {
      co_await asyncRuntime->sleepFor(delay, stopToken);
      co_await asyncRuntime->resumeOnCallbackExecutor(stopToken);

      if (auto const statePtr = weakStatePtr.lock(); statePtr != nullptr)
      {
        statePtr->handleDeadline();
      }
    }

    void handleDeadline()
    {
      if (shuttingDown || !active || !running || !deadlineScheduled)
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
      ++synchronizationRevision;

      if (deadlineScheduled)
      {
        deadlineTask.reset();
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

    async::Runtime& asyncRuntime;
    async::TaskHandle deadlineTask;
    LiveElapsedReader liveElapsedReader;
    AvailabilityChangedHandler availabilityChangedHandler;
    std::uint64_t synchronizationRevision = 0;
    bool active = false;
    bool running = false;
    bool restartAvailable = false;
    bool deadlineScheduled = false;
    bool shuttingDown = false;
  };

  PlaybackRestartDeadline::PlaybackRestartDeadline(async::Runtime& asyncRuntime,
                                                   LiveElapsedReader liveElapsedReader,
                                                   AvailabilityChangedHandler availabilityChangedHandler)
    : _sharedStatePtr{std::make_shared<SharedState>(asyncRuntime,
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
    _sharedStatePtr->start(elapsed);
  }

  void PlaybackRestartDeadline::resume(Elapsed const elapsed)
  {
    _sharedStatePtr->resume(elapsed);
  }

  void PlaybackRestartDeadline::pause(Elapsed const elapsed)
  {
    _sharedStatePtr->pause(elapsed);
  }

  void PlaybackRestartDeadline::seek(Elapsed const elapsed)
  {
    _sharedStatePtr->seek(elapsed);
  }

  void PlaybackRestartDeadline::currentTrackChanged(Elapsed const elapsed, bool const playing)
  {
    _sharedStatePtr->resetCurrent(elapsed, playing);
  }

  void PlaybackRestartDeadline::replaceSession(Elapsed const elapsed, bool const playing)
  {
    _sharedStatePtr->resetCurrent(elapsed, playing);
  }

  void PlaybackRestartDeadline::clearSession() noexcept
  {
    _sharedStatePtr->clearSession();
  }

  void PlaybackRestartDeadline::shutdown() noexcept
  {
    _sharedStatePtr->shutdown();
  }

  bool PlaybackRestartDeadline::isActive() const noexcept
  {
    return _sharedStatePtr->active;
  }

  bool PlaybackRestartDeadline::isRunning() const noexcept
  {
    return _sharedStatePtr->running;
  }

  bool PlaybackRestartDeadline::restartAvailable() const noexcept
  {
    return _sharedStatePtr->restartAvailable;
  }

  bool PlaybackRestartDeadline::hasScheduledDeadline() const noexcept
  {
    return _sharedStatePtr->deadlineScheduled;
  }
} // namespace ao::rt
