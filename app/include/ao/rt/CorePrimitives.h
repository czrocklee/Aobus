// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Executor.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/utility/StrongType.h>

#include <algorithm>
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
      auto const id = _nextId++;
      _handlers.push_back(Slot{.id = id, .handler = std::move(handler)});

      return Subscription{[this, id] { disconnect(id); }};
    }

    void emit(Args... args)
    {
      // Index loop over a snapshotted size: a handler may connect new handlers while we
      // emit. Unsubscribe only tombstones slots during emission so the active
      // callable stays alive until the outermost emit returns.
      auto const guard = EmitGuard{*this};
      auto const count = _handlers.size();

      for (std::size_t index = 0; index < count; ++index)
      {
        if (auto& slot = _handlers[index]; slot.connected && slot.handler)
        {
          slot.handler(args...);
        }
      }
    }

    void post(async::IExecutor& executor, std::decay_t<Args>... args)
    {
      executor.defer([this, ... args = std::move(args)] mutable { emit(args...); });
    }

    bool hasConnectedHandlers() const
    {
      return std::ranges::any_of(_handlers, [](auto const& slot) { return slot.connected; });
    }

  private:
    struct Slot final
    {
      std::size_t id = 0;
      std::move_only_function<void(Args...)> handler;
      bool connected = true;
    };

    class [[nodiscard]] EmitGuard final
    {
    public:
      explicit EmitGuard(Signal& owner)
        : _owner{owner}
      {
        ++_owner._emitDepth;
      }

      ~EmitGuard()
      {
        --_owner._emitDepth;

        if (_owner._emitDepth == 0 && _owner._needsCompact)
        {
          _owner.compactDisconnected();
        }
      }

      EmitGuard(EmitGuard const&) = delete;
      EmitGuard& operator=(EmitGuard const&) = delete;
      EmitGuard(EmitGuard&&) = delete;
      EmitGuard& operator=(EmitGuard&&) = delete;

    private:
      Signal& _owner;
    };

    void disconnect(std::size_t id)
    {
      for (auto& slot : _handlers)
      {
        if (slot.id == id)
        {
          slot.connected = false;
          _needsCompact = true;
          break;
        }
      }

      if (_emitDepth == 0 && _needsCompact)
      {
        compactDisconnected();
      }
    }

    void compactDisconnected()
    {
      for (auto it = _handlers.begin(); it != _handlers.end();)
      {
        if (!it->connected)
        {
          it = _handlers.erase(it);
        }
        else
        {
          ++it;
        }
      }

      _needsCompact = false;
    }

    std::deque<Slot> _handlers;
    std::size_t _nextId = 1;
    std::size_t _emitDepth = 0;
    bool _needsCompact = false;
  };
} // namespace ao::rt
