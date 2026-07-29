// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/AsyncTestSupport.h"

#include <ao/async/AsyncExceptionHandler.h>
#include <ao/async/Task.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  struct AsyncExceptionRecorder::Impl final
  {
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::vector<RecordedAsyncException> exceptions;
  };

  AsyncExceptionRecorder::AsyncExceptionRecorder()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  AsyncExceptionRecorder::~AsyncExceptionRecorder() = default;

  async::AsyncExceptionHandler AsyncExceptionRecorder::handler()
  {
    return [this](std::exception_ptr exceptionPtr, std::string_view const context)
    {
      auto const lock = std::scoped_lock{_implPtr->mutex};
      _implPtr->exceptions.push_back({.exceptionPtr = std::move(exceptionPtr), .context = std::string{context}});
      _implPtr->cv.notify_all();
    };
  }

  bool AsyncExceptionRecorder::waitForCount(std::size_t const count, std::chrono::milliseconds const timeout) const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(lock, timeout, [this, count] { return _implPtr->exceptions.size() >= count; });
  }

  std::vector<RecordedAsyncException> AsyncExceptionRecorder::snapshot() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return _implPtr->exceptions;
  }

  struct ControlledSleeper::Impl final
  {
    using StopCallback = std::stop_callback<std::function<void()>>;

    struct Entry final
    {
      std::uint64_t id = 0;
      Delay delay{};
      std::weak_ptr<boost::asio::steady_timer> timerPtr;
      bool active = false;
      // Driver methods observe an entry only after its stop callback is
      // registered, so cancellation cannot race publication.
      bool published = false;
      bool cancelled = false;
      std::thread::id startedOn = {};
      std::thread::id cancelledOn = {};
    };

    std::vector<Entry>::iterator entry(std::uint64_t const id) { return std::ranges::find(entries, id, &Entry::id); }

    std::vector<Delay> pendingDelaysLocked() const
    {
      auto delays = std::vector<Delay>{};

      for (auto const& candidate : entries)
      {
        if (candidate.published && candidate.active)
        {
          delays.push_back(candidate.delay);
        }
      }

      return delays;
    }

    async::Task<void> waitForSignal(Delay const delay, std::stop_token const stopToken)
    {
      auto executor = co_await boost::asio::this_coro::executor;
      auto timerPtr = std::make_shared<boost::asio::steady_timer>(executor);
      timerPtr->expires_at(std::chrono::steady_clock::time_point::max());
      auto const id = nextId.fetch_add(1);

      {
        auto const lock = std::scoped_lock{mutex};
        entries.push_back(Entry{
          .id = id, .delay = delay, .timerPtr = timerPtr, .active = true, .startedOn = std::this_thread::get_id()});
      }

      auto stopCallback = StopCallback{
        stopToken,
        [this, id, timerPtr]
        {
          bool wonCancellation = false;

          {
            auto const lock = std::scoped_lock{mutex};

            if (auto const it = entry(id); it != entries.end() && it->active)
            {
              it->cancelled = true;
              it->cancelledOn = std::this_thread::get_id();
              it->active = false;
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
        entry(id)->published = true;
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
    std::atomic_uint64_t nextId{1};
  };

  ControlledSleeper::ControlledSleeper()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  ControlledSleeper::~ControlledSleeper() = default;

  async::Task<void> ControlledSleeper::sleepFor(Delay const delay, std::stop_token const stopToken)
  {
    auto executor = co_await boost::asio::this_coro::executor;
    auto timerExecutor = boost::asio::make_strand(executor);
    co_await boost::asio::co_spawn(
      timerExecutor, _implPtr->waitForSignal(delay, stopToken), boost::asio::use_awaitable);
  }

  bool ControlledSleeper::waitForCallCount(std::size_t const count, std::chrono::milliseconds const timeout) const
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
    auto lock = std::unique_lock{_implPtr->mutex};
    _implPtr->cv.wait(
      lock, [this, index] { return _implPtr->entries.size() > index && _implPtr->entries[index].published; });
    auto const& entryValue = _implPtr->entries[index];
    return {.id = entryValue.id,
            .delay = entryValue.delay,
            .cancelled = entryValue.cancelled,
            .startedOn = entryValue.startedOn,
            .cancelledOn = entryValue.cancelledOn};
  }

  bool ControlledSleeper::waitForCancellation(std::size_t const index, std::chrono::milliseconds const timeout) const
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

  bool ControlledSleeper::fire(std::size_t const index)
  {
    auto timerPtr = std::shared_ptr<boost::asio::steady_timer>{};

    {
      auto const lock = std::scoped_lock{_implPtr->mutex};

      if (index >= _implPtr->entries.size() || !_implPtr->entries[index].published || !_implPtr->entries[index].active)
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

  bool ControlledSleeper::fireNext()
  {
    std::size_t index = 0;

    {
      auto lock = std::unique_lock{_implPtr->mutex};

      if (!_implPtr->cv.wait_for(lock,
                                 std::chrono::seconds{2},
                                 [this]
                                 {
                                   return std::ranges::any_of(_implPtr->entries,
                                                              [](Impl::Entry const& candidate)
                                                              { return candidate.published && candidate.active; });
                                 }))
      {
        return false;
      }

      auto const it = std::ranges::find_if(
        _implPtr->entries, [](Impl::Entry const& candidate) { return candidate.published && candidate.active; });
      index = static_cast<std::size_t>(it - _implPtr->entries.begin());
    }

    return fire(index);
  }

  bool ControlledSleeper::fireNext(Delay const delay)
  {
    std::size_t index = 0;

    {
      auto lock = std::unique_lock{_implPtr->mutex};

      if (!_implPtr->cv.wait_for(lock,
                                 std::chrono::seconds{2},
                                 [this, delay]
                                 {
                                   return std::ranges::any_of(
                                     _implPtr->entries,
                                     [delay](Impl::Entry const& candidate)
                                     { return candidate.published && candidate.active && candidate.delay == delay; });
                                 }))
      {
        return false;
      }

      auto const it =
        std::ranges::find_if(_implPtr->entries,
                             [delay](Impl::Entry const& candidate)
                             { return candidate.published && candidate.active && candidate.delay == delay; });
      index = static_cast<std::size_t>(it - _implPtr->entries.begin());
    }

    return fire(index);
  }

  bool ControlledSleeper::fireById(std::uint64_t const id)
  {
    std::size_t index = 0;

    {
      auto const lock = std::scoped_lock{_implPtr->mutex};
      auto const it = _implPtr->entry(id);

      if (it == _implPtr->entries.end())
      {
        return false;
      }

      index = static_cast<std::size_t>(it - _implPtr->entries.begin());
    }

    return fire(index);
  }

  std::uint64_t ControlledSleeper::lastScheduledId() const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    _implPtr->cv.wait(lock, [this] { return std::ranges::any_of(_implPtr->entries, &Impl::Entry::published); });
    auto const it = std::ranges::find_if(_implPtr->entries | std::views::reverse, &Impl::Entry::published);
    return it->id;
  }

  std::vector<ControlledSleeper::Delay> ControlledSleeper::pendingDelays() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return _implPtr->pendingDelaysLocked();
  }

  bool ControlledSleeper::waitForPendingDelays(std::vector<Delay> const& expected,
                                               std::chrono::milliseconds const timeout) const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(
      lock, timeout, [this, &expected] { return _implPtr->pendingDelaysLocked() == expected; });
  }

  bool ControlledSleeper::waitForPendingDelay(Delay const delay, std::chrono::milliseconds const timeout) const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(lock,
                                 timeout,
                                 [this, delay]
                                 {
                                   return std::ranges::any_of(
                                     _implPtr->entries,
                                     [delay](Impl::Entry const& candidate)
                                     { return candidate.published && candidate.active && candidate.delay == delay; });
                                 });
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
