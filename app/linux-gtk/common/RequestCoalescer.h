// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/ScopedRegistration.h>

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::gtk
{
  /**
   * Coalesces equal owner-affine requests while keeping callback interests
   * independently cancellable.
   *
   * request(), prefetch(), and complete() must run on the owning executor.
   * Request cancellation may run on any thread and may outlive the coalescer.
   * request() with an empty callback is intentionally equivalent to prefetch()
   * and returns an empty Request.
   * Completion removes its flight before callback fanout, so callbacks may
   * reenter request() for the same key. Synchronously destroying the coalescer
   * from a completion callback is not supported by the GTK ownership model.
   */
  template<typename Key, typename Value, typename Hash = std::hash<Key>, typename Equal = std::equal_to<Key>>
  class RequestCoalescer final
  {
  public:
    using Callback = std::move_only_function<void(Value const&)>;
    using Request = utility::ScopedRegistration;

    template<typename Start>
    Request request(Key const& key, Callback callback, Start&& start)
    {
      if (!callback)
      {
        prefetch(key, std::forward<Start>(start));
        return {};
      }

      auto interestPtr = std::make_shared<Interest>();

      if (auto const it = _flights.find(key); it != _flights.end())
      {
        it->second->waiters.push_back({.interestPtr = interestPtr, .callback = std::move(callback)});
      }
      else
      {
        auto flightPtr = std::make_shared<Flight>();
        flightPtr->waiters.push_back({.interestPtr = interestPtr, .callback = std::move(callback)});
        _flights.emplace(key, flightPtr);
        startFlight(key, flightPtr, std::forward<Start>(start));
      }

      return Request{[interestPtr] { interestPtr->active.store(false, std::memory_order_relaxed); }};
    }

    template<typename Start>
    void prefetch(Key const& key, Start&& start)
    {
      if (_flights.contains(key))
      {
        return;
      }

      auto flightPtr = std::make_shared<Flight>();
      _flights.emplace(key, flightPtr);
      startFlight(key, flightPtr, std::forward<Start>(start));
    }

    void complete(Key const& key, Value const& value)
    {
      auto const it = _flights.find(key);

      if (it == _flights.end())
      {
        return;
      }

      auto flightPtr = std::move(it->second);
      _flights.erase(it);
      auto firstException = std::exception_ptr{};

      for (auto& waiter : flightPtr->waiters)
      {
        if (!waiter.interestPtr->active.load(std::memory_order_relaxed))
        {
          continue;
        }

        try
        {
          waiter.callback(value);
        }
        catch (...)
        {
          if (!firstException)
          {
            firstException = std::current_exception();
          }
        }
      }

      if (firstException)
      {
        std::rethrow_exception(firstException);
      }
    }

  private:
    struct Interest final
    {
      std::atomic_bool active{true};
    };

    struct Waiter final
    {
      std::shared_ptr<Interest> interestPtr;
      Callback callback;
    };

    struct Flight final
    {
      std::vector<Waiter> waiters;
    };

    template<typename Start>
    void startFlight(Key const& key, std::shared_ptr<Flight> const& flightPtr, Start&& start)
    {
      try
      {
        std::invoke(std::forward<Start>(start));
      }
      catch (...)
      {
        if (auto const it = _flights.find(key); it != _flights.end() && it->second == flightPtr)
        {
          _flights.erase(it);
        }

        throw;
      }
    }

    std::unordered_map<Key, std::shared_ptr<Flight>, Hash, Equal> _flights;
  };
} // namespace ao::gtk
