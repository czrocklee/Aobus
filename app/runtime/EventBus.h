// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"

#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ao::app
{
  class EventBus final
  {
  public:
    template<class Event>
    Subscription subscribe(std::move_only_function<void(Event const&)> handler)
    {
      auto key = std::type_index(typeid(Event));
      auto& subscribers = _subscribers[key];
      auto index = subscribers.size();
      subscribers.push_back([h = std::move(handler)](void const* eventPtr) mutable
                            { h(*static_cast<Event const*>(eventPtr)); });

      return Subscription([this, key, index] { _subscribers[key][index] = {}; });
    }

    template<class Event>
    void publish(Event const& event)
    {
      auto it = _subscribers.find(std::type_index(typeid(Event)));
      if (it == _subscribers.end())
      {
        return;
      }

      for (auto& erased : it->second)
      {
        if (erased)
        {
          erased(&event);
        }
      }
    }

  private:
    using ErasedSubscriber = std::move_only_function<void(void const*)>;

    std::unordered_map<std::type_index, std::vector<ErasedSubscriber>> _subscribers;
  };
}
