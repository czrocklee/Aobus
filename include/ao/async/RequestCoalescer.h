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

namespace ao::async
{
  /**
   * Coalesces equal owner-affine requests while keeping callback interests
   * independently cancellable.
   *
   * request(), prefetch(), complete(), and clear() must run on the owning
   * executor. Request cancellation may run on any thread and may outlive the
   * coalescer. request() with an empty callback is intentionally equivalent to
   * prefetch() and returns an empty Request.
   *
   * Completion removes its flight before callback fanout, so callbacks may
   * reenter request() for the same key. A FlightToken identifies the exact
   * flight that started the work, so completion after clear() cannot retire a
   * replacement flight for the same key. clear() forgets flights but does not
   * cancel their external work; the owner must cancel that work before
   * releasing any state it may access.
   *
   * retainDependency() transfers a cancellable dependency to an exact flight.
   * Completion, clear(), or starter rollback releases that dependency. A stale
   * token rejects and immediately releases the supplied dependency.
   */
  template<typename Key, typename Value, typename Hash = std::hash<Key>, typename Equal = std::equal_to<Key>>
  class RequestCoalescer final
  {
  public:
    using Callback = std::move_only_function<void(Value const&)>;
    using Request = utility::ScopedRegistration;

    class FlightToken final
    {
    public:
      FlightToken(FlightToken const&) = default;
      FlightToken& operator=(FlightToken const&) = default;
      FlightToken(FlightToken&&) noexcept = default;
      FlightToken& operator=(FlightToken&&) noexcept = default;
      ~FlightToken() = default;

    private:
      FlightToken(Key key, std::weak_ptr<void> flightPtr)
        : _key{std::move(key)}, _flightPtr{std::move(flightPtr)}
      {
      }

      Key _key;
      std::weak_ptr<void> _flightPtr;

      friend class RequestCoalescer;
    };

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

    void complete(FlightToken const& token, Value const& value)
    {
      auto const tokenFlightPtr = token._flightPtr.lock();

      if (!tokenFlightPtr)
      {
        return;
      }

      auto const it = _flights.find(token._key);

      if (it == _flights.end() || it->second != tokenFlightPtr)
      {
        return;
      }

      auto flightPtr = std::move(it->second);
      _flights.erase(it);
      flightPtr->dependencies.clear();
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

    bool retainDependency(FlightToken const& token, utility::ScopedRegistration dependency)
    {
      auto const tokenFlightPtr = token._flightPtr.lock();

      if (!tokenFlightPtr)
      {
        return false;
      }

      auto const it = _flights.find(token._key);

      if (it == _flights.end() || it->second != tokenFlightPtr)
      {
        return false;
      }

      it->second->dependencies.push_back(std::move(dependency));
      return true;
    }

    void clear() noexcept { _flights.clear(); }

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
      std::vector<utility::ScopedRegistration> dependencies;
    };

    template<typename Start>
    void startFlight(Key const& key, std::shared_ptr<Flight> const& flightPtr, Start&& start)
    {
      try
      {
        std::invoke(std::forward<Start>(start), FlightToken{key, std::weak_ptr<void>{flightPtr}});
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
} // namespace ao::async
