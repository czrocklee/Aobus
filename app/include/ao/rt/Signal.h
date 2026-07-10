// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Subscription.h"
#include <ao/async/Executor.h>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ao::rt
{
  template<typename... Args>
  class Signal final
  {
  public:
    Signal();
    ~Signal();

    Signal(Signal const&) = delete;
    Signal& operator=(Signal const&) = delete;
    Signal(Signal&&) = delete;
    Signal& operator=(Signal&&) = delete;

    Subscription connect(std::move_only_function<void(Args...)> handler);
    void emit(Args... args);
    void post(async::Executor& executor, std::decay_t<Args>... args);
    bool hasConnectedHandlers() const;
    void disconnectAll();

  private:
    struct Slot final
    {
      std::size_t id = 0;
      std::move_only_function<void(Args...)> handler;
      bool connected = true;
    };

    class State final
    {
    public:
      std::size_t connect(std::move_only_function<void(Args...)> handler)
      {
        if (!_active)
        {
          return 0;
        }

        auto const id = _nextId++;
        _handlers.push_back(Slot{.id = id, .handler = std::move(handler)});
        return id;
      }

      void emit(Args... args)
      {
        if (!_active)
        {
          return;
        }

        // Index loop over a snapshotted size: a handler may connect new
        // handlers while we emit. Unsubscribe only tombstones slots during
        // emission so the active callable stays alive until the outermost emit
        // returns.
        auto const guard = EmitGuard{*this};
        auto const count = _handlers.size();

        for (std::size_t index = 0; index < count && _active; ++index)
        {
          if (auto& slot = _handlers[index]; slot.connected && slot.handler)
          {
            slot.handler(args...);
          }
        }
      }

      bool hasConnectedHandlers() const
      {
        return _active && std::ranges::any_of(_handlers, [](auto const& slot) { return slot.connected; });
      }

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

        compactIfIdle();
      }

      void disconnectAll()
      {
        tombstoneHandlers();
        compactIfIdle();
      }

      void close()
      {
        _active = false;
        tombstoneHandlers();
        compactIfIdle();
      }

    private:
      class [[nodiscard]] EmitGuard final
      {
      public:
        explicit EmitGuard(State& owner)
          : _owner{owner}
        {
          ++_owner._emitDepth;
        }

        ~EmitGuard()
        {
          --_owner._emitDepth;
          _owner.compactIfIdle();
        }

        EmitGuard(EmitGuard const&) = delete;
        EmitGuard& operator=(EmitGuard const&) = delete;
        EmitGuard(EmitGuard&&) = delete;
        EmitGuard& operator=(EmitGuard&&) = delete;

      private:
        State& _owner;
      };

      void tombstoneHandlers()
      {
        for (auto& slot : _handlers)
        {
          slot.connected = false;
        }

        _needsCompact = !_handlers.empty();
      }

      void compactIfIdle()
      {
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
      bool _active = true;
    };

    std::shared_ptr<State> _statePtr;
  };

  template<typename... Args>
  Signal<Args...>::Signal()
    : _statePtr{std::make_shared<State>()}
  {
  }

  template<typename... Args>
  Signal<Args...>::~Signal()
  {
    _statePtr->close();
  }

  template<typename... Args>
  Subscription Signal<Args...>::connect(std::move_only_function<void(Args...)> handler)
  {
    auto const statePtr = _statePtr;
    auto const id = statePtr->connect(std::move(handler));

    if (id == 0)
    {
      return {};
    }

    auto const weakStatePtr = std::weak_ptr<State>{statePtr};
    return Subscription{[weakStatePtr, id]
                        {
                          if (auto statePtr = weakStatePtr.lock(); statePtr != nullptr)
                          {
                            statePtr->disconnect(id);
                          }
                        }};
  }

  template<typename... Args>
  void Signal<Args...>::emit(Args... args)
  {
    auto const statePtr = _statePtr;
    statePtr->emit(args...);
  }

  template<typename... Args>
  void Signal<Args...>::post(async::Executor& executor, std::decay_t<Args>... args)
  {
    auto const weakStatePtr = std::weak_ptr<State>{_statePtr};
    executor.defer(
      [weakStatePtr, ... args = std::move(args)] mutable
      {
        if (auto statePtr = weakStatePtr.lock(); statePtr != nullptr)
        {
          statePtr->emit(args...);
        }
      });
  }

  template<typename... Args>
  bool Signal<Args...>::hasConnectedHandlers() const
  {
    return _statePtr->hasConnectedHandlers();
  }

  template<typename... Args>
  void Signal<Args...>::disconnectAll()
  {
    _statePtr->disconnectAll();
  }
} // namespace ao::rt
