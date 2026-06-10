// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/async/Executor.h>
#include <ao/utility/StrongType.h>
#include <ao/utility/Subscription.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt
{
  using ViewId = utility::StrongType<std::uint64_t, struct ViewIdTag>;
  using NotificationId = utility::StrongType<std::uint64_t, struct NotificationIdTag>;

  inline constexpr auto kInvalidViewId = ViewId{0};
  inline constexpr auto kInvalidNotificationId = NotificationId{0};

  inline constexpr auto kAllTracksListId = ListId{std::numeric_limits<std::uint32_t>::max()};

  struct Range final
  {
    std::size_t start = 0;
    std::size_t count = 0;
  };

  using Subscription = utility::Subscription;

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
      for (auto& handler : _handlers)
      {
        if (handler)
        {
          handler(args...);
        }
      }
    }

    void post(async::IExecutor& executor, std::decay_t<Args>... args)
    {
      executor.defer(
        [this, ... args = std::move(args)] mutable
        {
          for (auto& handler : _handlers)
          {
            if (handler)
            {
              handler(args...);
            }
          }
        });
    }

  private:
    std::vector<std::move_only_function<void(Args...)>> _handlers;
  };
}
