// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/BackendObserverDispatcher.h"

#include <ao/Contract.h>

#include <condition_variable>
#include <latch>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace ao::audio::backend::detail
{
  BackendObserverDispatcher::Admission BackendObserverDispatcher::claimPublisher(std::stop_token stop)
  {
    auto lock = std::unique_lock{_mutex};

    if (stop.stop_requested())
    {
      return Admission::Cancelled;
    }

    auto const caller = std::this_thread::get_id();

    if (_publisher == caller)
    {
      return Admission::CurrentPublisher;
    }

    if (_publisher != std::thread::id{})
    {
      auto const idle = [this] { return _publisher == std::thread::id{}; };

      if (stop.stop_possible())
      {
        if (_waitObserver != nullptr)
        {
          // The observer cannot reacquire _mutex until the wait below releases it.
          std::exchange(_waitObserver, nullptr)->notify_one();
        }

        if (!_publisherChanged.wait(lock, stop, idle) || stop.stop_requested())
        {
          return Admission::Cancelled;
        }
      }
      else
      {
        _publisherChanged.wait(lock, idle);
      }
    }

    _publisher = caller;
    return Admission::NewPublisher;
  }

  void BackendObserverDispatcher::observeNextPublisherWait(std::latch& armed)
  {
    auto entered = std::condition_variable{};
    auto lock = std::unique_lock{_mutex};
    AO_EXPECTS(_waitObserver == nullptr, "Only one observer can await the next publisher wait");
    _waitObserver = &entered;
    armed.count_down();
    entered.wait(lock, [&] { return _waitObserver != &entered; });
  }

  void BackendObserverDispatcher::finishPublication() noexcept
  {
    {
      auto const lock = std::scoped_lock{_mutex};
      _publisher = {};
    }
    _publisherChanged.notify_all();
  }

  bool BackendObserverDispatcher::isDispatchingOnThisThread() const noexcept
  {
    auto const lock = std::scoped_lock{_mutex};
    return _publisher == std::this_thread::get_id();
  }
} // namespace ao::audio::backend::detail
