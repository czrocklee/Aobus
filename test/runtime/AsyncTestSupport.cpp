// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/runtime/AsyncTestSupport.h"

#include <ao/Contract.h>
#include <ao/async/Task.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stop_token>
#include <thread>
#include <tuple>
#include <vector>

namespace ao::rt::test
{
  ScopedTestDeadline::ScopedTestDeadline(std::source_location location)
    : _watchdog{[finished = _finished.get_future(), location]
                {
                  if (finished.wait_for(std::chrono::seconds{30}) != std::future_status::ready)
                  {
                    AO_FATAL_AT(location, "Test process deadline exceeded");
                  }
                }}
  {
  }

  ScopedTestDeadline::~ScopedTestDeadline()
  {
    _finished.set_value();
  }

  struct ControlledSleeper::Impl final
  {
    struct Entry final
    {
      Delay delay{};
      std::weak_ptr<boost::asio::steady_timer> timerPtr;
      bool active = false;
      // Drivers may observe the entry only after its stop callback is registered.
      bool published = false;
      bool cancelled = false;
      std::thread::id startedOn = {};
      std::thread::id cancelledOn = {};

      bool isPending() const { return published && active; }
    };

    std::optional<std::size_t> tryWaitForPendingIndex(std::optional<Delay> const optDelay,
                                                      std::chrono::milliseconds const timeout = std::chrono::seconds{
                                                        10}) const
    {
      auto lock = std::unique_lock{mutex};
      auto optIndex = std::optional<std::size_t>{};
      auto matches = [optDelay](Entry const& candidate)
      { return candidate.isPending() && (!optDelay || candidate.delay == *optDelay); };

      std::ignore = cv.wait_for(lock,
                                timeout,
                                [&]
                                {
                                  auto const it = std::ranges::find_if(entries, matches);

                                  if (it == entries.end())
                                  {
                                    return false;
                                  }

                                  optIndex = static_cast<std::size_t>(it - entries.begin());
                                  return true;
                                });
      return optIndex;
    }

    std::vector<Delay> pendingDelaysLocked() const
    {
      auto delays = std::vector<Delay>{};

      for (auto const& candidate : entries)
      {
        if (candidate.isPending())
        {
          delays.push_back(candidate.delay);
        }
      }

      return delays;
    }

    async::Task<void> waitForSignalAsync(Delay const delay, std::stop_token const stopToken)
    {
      auto executor = co_await boost::asio::this_coro::executor;
      auto timerPtr = std::make_shared<boost::asio::steady_timer>(executor);
      timerPtr->expires_at(std::chrono::steady_clock::time_point::max());
      std::size_t index = 0;

      {
        auto const lock = std::scoped_lock{mutex};
        index = entries.size();
        entries.push_back(
          Entry{.delay = delay, .timerPtr = timerPtr, .active = true, .startedOn = std::this_thread::get_id()});
      }

      // Entries are append-only, so this index remains valid across registrations.
      auto stopCallback = std::stop_callback{
        stopToken,
        [this, index, timerPtr]
        {
          bool wonCancellation = false;

          {
            auto const lock = std::scoped_lock{mutex};

            if (auto& call = entries[index]; call.active)
            {
              call.cancelled = true;
              call.cancelledOn = std::this_thread::get_id();
              call.active = false;
              wonCancellation = true;
            }
          }

          cv.notify_all();

          if (wonCancellation)
          {
            boost::asio::dispatch(timerPtr->get_executor(), [timerPtr] { std::ignore = timerPtr->cancel(); });
          }
        }};

      {
        auto const lock = std::scoped_lock{mutex};
        entries[index].published = true;
      }

      cv.notify_all();

      if (stopToken.stop_requested())
      {
        co_return;
      }

      try
      {
        co_await timerPtr->async_wait(boost::asio::use_awaitable);
      }
      catch (boost::system::system_error const& error)
      {
        if (error.code() != boost::asio::error::operation_aborted)
        {
          throw;
        }
      }
    }

    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::vector<Entry> entries;
  };

  ControlledSleeper::ControlledSleeper()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  ControlledSleeper::~ControlledSleeper() = default;

  async::Task<void> ControlledSleeper::sleepForAsync(Delay const delay, std::stop_token const stopToken)
  {
    auto executor = co_await boost::asio::this_coro::executor;
    auto timerExecutor = boost::asio::make_strand(executor);
    co_await boost::asio::co_spawn(
      timerExecutor, _implPtr->waitForSignalAsync(delay, stopToken), boost::asio::use_awaitable);
  }

  bool ControlledSleeper::tryWaitForCallCount(std::size_t const count, std::chrono::milliseconds const timeout) const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(lock,
                                 timeout,
                                 [this, count]
                                 {
                                   if (_implPtr->entries.size() < count)
                                   {
                                     return false;
                                   }

                                   for (std::size_t index = 0; index < count; ++index)
                                   {
                                     if (!_implPtr->entries[index].published)
                                     {
                                       return false;
                                     }
                                   }

                                   return true;
                                 });
  }

  std::size_t ControlledSleeper::callCount() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return static_cast<std::size_t>(std::ranges::count(_implPtr->entries, true, &Impl::Entry::published));
  }

  ControlledSleeper::Call ControlledSleeper::call(std::size_t const index) const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    REQUIRE(index < _implPtr->entries.size());
    auto const& entryValue = _implPtr->entries[index];
    REQUIRE(entryValue.published);
    return {.delay = entryValue.delay,
            .cancelled = entryValue.cancelled,
            .startedOn = entryValue.startedOn,
            .cancelledOn = entryValue.cancelledOn};
  }

  bool ControlledSleeper::tryWaitForCancellation(std::size_t const index, std::chrono::milliseconds const timeout) const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(lock,
                                 timeout,
                                 [this, index]
                                 {
                                   return _implPtr->entries.size() > index && _implPtr->entries[index].published &&
                                          _implPtr->entries[index].cancelled;
                                 });
  }

  bool ControlledSleeper::tryFire(std::size_t const index)
  {
    auto timerPtr = std::shared_ptr<boost::asio::steady_timer>{};

    {
      auto const lock = std::scoped_lock{_implPtr->mutex};

      if (index >= _implPtr->entries.size() || !_implPtr->entries[index].isPending())
      {
        return false;
      }

      timerPtr = _implPtr->entries[index].timerPtr.lock();

      if (timerPtr == nullptr)
      {
        _implPtr->entries[index].active = false;
        return false;
      }

      _implPtr->entries[index].active = false;
    }

    boost::asio::dispatch(timerPtr->get_executor(), [timerPtr] { std::ignore = timerPtr->cancel(); });
    return true;
  }

  bool ControlledSleeper::tryFireNext()
  {
    auto const optIndex = _implPtr->tryWaitForPendingIndex(std::nullopt);
    return optIndex && tryFire(*optIndex);
  }

  bool ControlledSleeper::tryFireNext(Delay const delay)
  {
    auto const optIndex = _implPtr->tryWaitForPendingIndex(delay);
    return optIndex && tryFire(*optIndex);
  }

  std::vector<ControlledSleeper::Delay> ControlledSleeper::pendingDelays() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return _implPtr->pendingDelaysLocked();
  }

  bool ControlledSleeper::tryWaitForPendingDelays(std::vector<Delay> const& expected,
                                                  std::chrono::milliseconds const timeout) const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(
      lock, timeout, [this, &expected] { return _implPtr->pendingDelaysLocked() == expected; });
  }

  bool ControlledSleeper::tryWaitForPendingDelay(Delay const delay, std::chrono::milliseconds const timeout) const
  {
    return _implPtr->tryWaitForPendingIndex(delay, timeout).has_value();
  }

  struct AsyncBarrier::Impl final
  {
    bool released = false;
    std::mutex mutex;
    std::condition_variable cv;
  };

  AsyncBarrier::AsyncBarrier()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  AsyncBarrier::~AsyncBarrier() = default;

  void AsyncBarrier::wait()
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    _implPtr->cv.wait(lock, [this] { return _implPtr->released; });
  }

  void AsyncBarrier::release()
  {
    {
      auto const lock = std::scoped_lock{_implPtr->mutex};
      _implPtr->released = true;
    }

    _implPtr->cv.notify_all();
  }
} // namespace ao::rt::test
