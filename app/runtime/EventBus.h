// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "BusLog.h"
#include "CorePrimitives.h"

#include <ao/utility/Log.h>

#include <cstddef>
#include <functional>
#include <source_location>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ao::app
{
  class EventBus final
  {
  public:
    template<class Event>
    Subscription subscribe(std::move_only_function<void(Event const&)> handler,
                           std::source_location const src = std::source_location::current())
    {
      auto key = std::type_index(typeid(Event));
      auto& subscribers = _subscribers[key];
      auto index = subscribers.size();
      subscribers.push_back([h = std::move(handler)](void const* eventPtr) mutable
                            { h(*static_cast<Event const*>(eventPtr)); });

      APP_LOG_DEBUG("evt ⊕ {} ({}:{})", detail::busTypeName<Event>(), detail::shortFileName(src), src.line());

      return Subscription([this, key, index] { _subscribers[key][index] = {}; });
    }

    template<class Event>
    void publish(Event const& event, std::source_location const src = std::source_location::current())
    {
      auto it = _subscribers.find(std::type_index(typeid(Event)));
      if (it == _subscribers.end())
      {
        APP_LOG_TRACE(
          "evt ✉ {} → 0 listeners ({}:{})", detail::busTypeName<Event>(), detail::shortFileName(src), src.line());
        return;
      }

      std::size_t count = 0;
      for (auto& erased : it->second)
      {
        if (erased)
        {
          erased(&event);
          ++count;
        }
      }

      APP_LOG_TRACE(
        "evt ✉ {} → {} listeners ({}:{})", detail::busTypeName<Event>(), count, detail::shortFileName(src), src.line());
    }

  private:
    using ErasedSubscriber = std::move_only_function<void(void const*)>;

    std::unordered_map<std::type_index, std::vector<ErasedSubscriber>> _subscribers;
  };
}
