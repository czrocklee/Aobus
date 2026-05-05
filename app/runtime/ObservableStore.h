// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"

#include <cstdint>
#include <vector>

namespace ao::app
{
  template<class T>
  class ObservableStore final : public IReadOnlyStore<T>
  {
  public:
    ObservableStore() = default;
    explicit ObservableStore(T initial)
      : _value{std::move(initial)}
    {
    }

    T snapshot() const override { return _value; }

    Subscription subscribe(std::move_only_function<void(T const&)> handler,
                           StoreDeliveryMode mode = StoreDeliveryMode::ReplayCurrent) override
    {
      if (mode == StoreDeliveryMode::ReplayCurrent)
      {
        handler(_value);
      }

      auto index = _subscribers.size();
      _subscribers.push_back(std::move(handler));

      return Subscription{[this, index] { _subscribers[index] = {}; }};
    }

    void update(T value)
    {
      _value = std::move(value);
      ++_revision;

      for (auto& sub : _subscribers)
      {
        if (sub)
        {
          sub(_value);
        }
      }
    }

    std::uint64_t revision() const noexcept { return _revision; }

  private:
    T _value{};
    std::uint64_t _revision = 0;
    std::vector<std::move_only_function<void(T const&)>> _subscribers;
  };
}
