// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <functional>
#include <utility>

namespace ao::utility
{
  class Subscription final
  {
  public:
    Subscription() = default;

    explicit Subscription(std::move_only_function<void()> unsubscribe)
      : _unsubscribe{std::move(unsubscribe)}
    {
    }

    ~Subscription()
    {
      if (_unsubscribe)
      {
        _unsubscribe();
      }
    }

    Subscription(Subscription const&) = delete;
    Subscription& operator=(Subscription const&) = delete;

    Subscription(Subscription&&) noexcept = default;
    Subscription& operator=(Subscription&&) noexcept = default;

    void reset()
    {
      if (_unsubscribe)
      {
        auto unsub = std::move(_unsubscribe);
        unsub();
      }
    }

    explicit operator bool() const noexcept { return static_cast<bool>(_unsubscribe); }

  private:
    std::move_only_function<void()> _unsubscribe;
  };
} // namespace ao::utility
