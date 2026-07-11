// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/rt/PlaybackSessionSaveService.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

namespace ao::rt
{
  struct PlaybackSessionSaveService::Impl final : std::enable_shared_from_this<Impl>
  {
    enum class ScheduledSave : std::uint8_t
    {
      None,
      DirtyDebounce,
      Retry,
    };

    Impl(Port portValue, Scheduler& schedulerValue, Timing const timingValue)
      : port{std::move(portValue)}
      , scheduler{schedulerValue}
      , timing{timingValue}
      , nextRetryDelay{timing.initialRetryDelay}
    {
      if (!port.subscribeDirty)
      {
        throwException<Exception>("Playback session save service requires a dirty subscriber");
      }

      if (!port.save)
      {
        throwException<Exception>("Playback session save service requires a save operation");
      }

      if (timing.dirtyDebounceDelay <= Delay::zero() || timing.initialRetryDelay <= Delay::zero() ||
          timing.maximumRetryDelay <= Delay::zero() || timing.initialRetryDelay > timing.maximumRetryDelay)
      {
        throwException<Exception>("Playback session save service timing must be positive and bounded");
      }
    }

    void start()
    {
      if (started || shuttingDown)
      {
        return;
      }

      started = true;
      refreshDirtySubscription();
    }

    void refreshDirtySubscription()
    {
      dirtySubscription.reset();
      auto const weakSelfPtr = weak_from_this();
      dirtySubscription = port.subscribeDirty(
        [weakSelfPtr]
        {
          if (auto const selfPtr = weakSelfPtr.lock(); selfPtr)
          {
            selfPtr->handleDirty();
          }
        });
    }

    void handleDirty()
    {
      if (!started || shuttingDown || scheduledSave == ScheduledSave::Retry)
      {
        return;
      }

      scheduleSave(ScheduledSave::DirtyDebounce, timing.dirtyDebounceDelay);
    }

    void saveImmediate()
    {
      if (shuttingDown)
      {
        return;
      }

      cancelScheduledSave();

      if (auto const result = port.save(); !result)
      {
        scheduleRetry();
        return;
      }

      handleSaveSucceeded();
    }

    void handleSaveSucceeded()
    {
      cancelScheduledSave();
      nextRetryDelay = timing.initialRetryDelay;

      if (started && !shuttingDown)
      {
        // A successful save acknowledges only the revision it captured. Re-subscribing
        // uses runtime's immediate dirty replay to detect a newer still-dirty revision
        // without waiting for a clean-to-dirty transition that will never occur.
        refreshDirtySubscription();
      }
    }

    void scheduleRetry()
    {
      auto const retryDelay = nextRetryDelay;

      if (nextRetryDelay >= timing.maximumRetryDelay / 2)
      {
        nextRetryDelay = timing.maximumRetryDelay;
      }
      else
      {
        nextRetryDelay = std::min(nextRetryDelay * 2, timing.maximumRetryDelay);
      }

      scheduleSave(ScheduledSave::Retry, retryDelay);
    }

    void scheduleSave(ScheduledSave const kind, Delay const delay)
    {
      cancelScheduledSave();
      scheduledSave = kind;
      auto const callbackGeneration = scheduleGeneration;
      auto const weakSelfPtr = weak_from_this();
      scheduledTask = scheduler.schedule(delay,
                                         [weakSelfPtr, callbackGeneration, kind]
                                         {
                                           if (auto const selfPtr = weakSelfPtr.lock(); selfPtr)
                                           {
                                             selfPtr->handleScheduledSave(callbackGeneration, kind);
                                           }
                                         });
    }

    void handleScheduledSave(std::uint64_t const callbackGeneration, ScheduledSave const kind)
    {
      if (shuttingDown || callbackGeneration != scheduleGeneration || scheduledSave != kind)
      {
        return;
      }

      cancelScheduledSave();
      saveImmediate();
    }

    void cancelScheduledSave() noexcept
    {
      ++scheduleGeneration;
      scheduledSave = ScheduledSave::None;
      scheduledTask.reset();
    }

    Result<> shutdown()
    {
      if (shuttingDown)
      {
        return {};
      }

      shuttingDown = true;
      dirtySubscription.reset();
      cancelScheduledSave();
      return port.save();
    }

    void dispose() noexcept
    {
      if (shuttingDown)
      {
        return;
      }

      shuttingDown = true;
      dirtySubscription.reset();
      cancelScheduledSave();
    }

    Port port;
    Scheduler& scheduler;
    Timing timing;
    Delay nextRetryDelay;
    Subscription dirtySubscription;
    Subscription scheduledTask;
    std::uint64_t scheduleGeneration = 0;
    ScheduledSave scheduledSave = ScheduledSave::None;
    bool started = false;
    bool shuttingDown = false;
  };

  PlaybackSessionSaveService::PlaybackSessionSaveService(Port port, Scheduler& scheduler)
    : PlaybackSessionSaveService{std::move(port), scheduler, Timing{}}
  {
  }

  PlaybackSessionSaveService::PlaybackSessionSaveService(Port port, Scheduler& scheduler, Timing const timing)
    : _implPtr{std::make_shared<Impl>(std::move(port), scheduler, timing)}
  {
  }

  PlaybackSessionSaveService::~PlaybackSessionSaveService()
  {
    _implPtr->dispose();
  }

  void PlaybackSessionSaveService::start()
  {
    _implPtr->start();
  }

  void PlaybackSessionSaveService::saveSignificantEvent()
  {
    _implPtr->saveImmediate();
  }

  void PlaybackSessionSaveService::savePeriodic()
  {
    _implPtr->saveImmediate();
  }

  Result<> PlaybackSessionSaveService::shutdown()
  {
    return _implPtr->shutdown();
  }
} // namespace ao::rt
