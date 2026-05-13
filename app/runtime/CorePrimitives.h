// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/utility/TaggedInteger.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ao::rt
{
  using ViewId = ao::utility::TaggedInteger<std::uint64_t, struct ViewIdTag>;
  using NotificationId = ao::utility::TaggedInteger<std::uint64_t, struct NotificationIdTag>;

  struct Range final
  {
    std::size_t start = 0;
    std::size_t count = 0;
  };

  class Subscription final
  {
  public:
    Subscription() = default;

    explicit Subscription(std::move_only_function<void()> unsubscribe)
      : _unsubscribe{std::move(unsubscribe)}
    {
    }

    Subscription(Subscription const&) = delete;
    Subscription& operator=(Subscription const&) = delete;

    Subscription(Subscription&&) noexcept = default;
    Subscription& operator=(Subscription&&) noexcept = default;

    ~Subscription()
    {
      if (_unsubscribe)
      {
        _unsubscribe();
      }
    }

    void reset()
    {
      if (_unsubscribe)
      {
        _unsubscribe();
        _unsubscribe = {};
      }
    }

    explicit operator bool() const noexcept { return static_cast<bool>(_unsubscribe); }

  private:
    std::move_only_function<void()> _unsubscribe;
  };

  class IControlExecutor
  {
  public:
    virtual ~IControlExecutor() = default;

    virtual bool isCurrent() const noexcept = 0;

    // Thread-safe: enqueue and wake the control thread (e.g. for cross-thread callbacks).
    virtual void dispatch(std::move_only_function<void()> task) = 0;

    // Always deferred: run in the next idle iteration, even from the control thread.
    virtual void defer(std::move_only_function<void()> task) = 0;
  };

  template<typename... Args>
  class Signal final
  {
  public:
    Subscription connect(std::move_only_function<void(Args...)> handler)
    {
      _handlers.push_back(std::move(handler));
      std::size_t const index = _handlers.size() - 1;

      return Subscription{[this, index] { _handlers[index] = {}; }};
    }

    void emit(Args... args)
    {
      for (auto& h : _handlers)
      {
        if (h)
        {
          h(args...);
        }
      }
    }

    void post(IControlExecutor& executor, std::decay_t<Args>... args)
    {
      executor.defer(
        [this, ... args = std::move(args)]() mutable
        {
          for (auto& h : _handlers)
          {
            if (h)
            {
              h(args...);
            }
          }
        });
    }

  private:
    std::vector<std::move_only_function<void(Args...)>> _handlers;
  };
}
