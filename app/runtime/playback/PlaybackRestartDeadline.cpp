// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackRestartDeadline.h"

#include <ao/Contract.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <utility>

namespace ao::rt
{
  struct PlaybackRestartDeadline::State final
  {
    State(async::Runtime& asyncRuntime,
          LiveElapsedReader liveElapsedReader,
          AvailabilityChangedHandler availabilityChangedHandler)
      : asyncRuntime{asyncRuntime}
      , liveElapsedReader{std::move(liveElapsedReader)}
      , availabilityChangedHandler{std::move(availabilityChangedHandler)}
    {
      AO_EXPECTS(this->liveElapsedReader, "Playback restart deadline requires a live elapsed reader");
      AO_EXPECTS(this->availabilityChangedHandler, "Playback restart deadline requires an availability handler");
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
      deadlineTask = asyncRuntime.spawnCancellable(
        [asyncRuntime = &asyncRuntime, state = this, delay](std::stop_token const stopToken)
        { return waitForDeadline(asyncRuntime, state, delay, stopToken); });
    }

    static async::Task<void> waitForDeadline(async::Runtime* asyncRuntime,
                                             State* state,
                                             Elapsed const delay,
                                             std::stop_token const stopToken)
    {
      co_await asyncRuntime->sleepFor(delay, stopToken);
      co_await asyncRuntime->resumeOnCallbackExecutor(stopToken);
      state->handleDeadline();
    }

    void handleDeadline()
    {
      if (shuttingDown || !active || !running || !deadlineTask)
      {
        return;
      }

      deadlineTask.reset();
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
      deadlineTask.reset();
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
    bool shuttingDown = false;
  };

  PlaybackRestartDeadline::PlaybackRestartDeadline(async::Runtime& asyncRuntime,
                                                   LiveElapsedReader liveElapsedReader,
                                                   AvailabilityChangedHandler availabilityChangedHandler)
    : _statePtr{
        std::make_unique<State>(asyncRuntime, std::move(liveElapsedReader), std::move(availabilityChangedHandler))}
  {
  }

  PlaybackRestartDeadline::~PlaybackRestartDeadline()
  {
    shutdown();
  }

  void PlaybackRestartDeadline::start(Elapsed const elapsed)
  {
    _statePtr->start(elapsed);
  }

  void PlaybackRestartDeadline::resume(Elapsed const elapsed)
  {
    _statePtr->resume(elapsed);
  }

  void PlaybackRestartDeadline::pause(Elapsed const elapsed)
  {
    _statePtr->pause(elapsed);
  }

  void PlaybackRestartDeadline::seek(Elapsed const elapsed)
  {
    _statePtr->seek(elapsed);
  }

  void PlaybackRestartDeadline::replaceSession(Elapsed const elapsed, bool const playing)
  {
    _statePtr->resetCurrent(elapsed, playing);
  }

  void PlaybackRestartDeadline::clearSession() noexcept
  {
    _statePtr->clearSession();
  }

  void PlaybackRestartDeadline::shutdown() noexcept
  {
    _statePtr->shutdown();
  }

  bool PlaybackRestartDeadline::isActive() const noexcept
  {
    return _statePtr->active;
  }

  bool PlaybackRestartDeadline::isRunning() const noexcept
  {
    return _statePtr->running;
  }

  bool PlaybackRestartDeadline::restartAvailable() const noexcept
  {
    return _statePtr->restartAvailable;
  }

  bool PlaybackRestartDeadline::hasScheduledDeadline() const noexcept
  {
    return static_cast<bool>(_statePtr->deadlineTask);
  }
} // namespace ao::rt
