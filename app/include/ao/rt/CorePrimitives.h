// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Executor.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/utility/StrongType.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>

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

  using Subscription = utility::ScopedRegistration;

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
      // Index loop over a snapshotted size: a handler may connect new handlers while we
      // emit. Storage is a deque so push_back never relocates the handler currently
      // executing; the new handlers only take part starting with the next emission.
      auto const count = _handlers.size();

      for (std::size_t index = 0; index < count; ++index)
      {
        if (_handlers[index])
        {
          _handlers[index](args...);
        }
      }
    }

    void post(async::IExecutor& executor, std::decay_t<Args>... args)
    {
      executor.defer([this, ... args = std::move(args)] mutable { emit(args...); });
    }

  private:
    std::deque<std::move_only_function<void(Args...)>> _handlers;
  };
} // namespace ao::rt
