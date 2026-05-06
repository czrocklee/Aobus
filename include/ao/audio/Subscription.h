// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <functional>

namespace ao::audio
{
  /**
   * @brief A move-only handle that unsubscribes a listener when destroyed.
   */
  class Subscription final
  {
  public:
    Subscription() = default;
    explicit Subscription(std::move_only_function<void()> unsub)
      : _unsub(std::move(unsub))
    {
    }

    ~Subscription()
    {
      if (_unsub)
      {
        _unsub();
      }
    }

    Subscription(Subscription const&) = delete;
    Subscription& operator=(Subscription const&) = delete;

    Subscription(Subscription&&) noexcept = default;
    Subscription& operator=(Subscription&&) noexcept = default;

    /**
     * @brief Manually trigger unsubscription.
     */
    void reset()
    {
      if (_unsub)
      {
        auto unsub = std::move(_unsub);
        unsub();
      }
    }

    /**
     * @brief Returns true if the subscription is active.
     */
    explicit operator bool() const noexcept { return static_cast<bool>(_unsub); }

  private:
    std::move_only_function<void()> _unsub;
  };
} // namespace ao::audio
