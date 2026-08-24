// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Contract.h>
#include <ao/async/Executor.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <source_location>
#include <thread>
#include <type_traits>
#include <utility>

namespace ao::async
{
  /**
   * Multicast event source with subscription-scoped disconnection.
   *
   * An observer is told about something that already happened, so a failure it
   * reports carries no decision the publisher could act on -- the state change
   * cannot be undone. The owning emission boundary diagnoses and aborts an
   * escaping exception rather than stepping over a diverged observer.
   *
   * The signal and every subscription are confined to the thread that constructs
   * the signal. `post` is the cross-thread delivery path, and its executor must
   * deliver on that owner thread. `emit` provides the non-throwing terminal
   * boundary. `post` may still fail before executor admission while it constructs
   * or submits the owned payload.
   */
  template<typename... Args>
  class Signal final
  {
  public:
    using Handler = compat::MoveOnlyFunction<void(Args...)>;

    Signal();
    ~Signal();

    Signal(Signal const&) = delete;
    Signal& operator=(Signal const&) = delete;
    Signal(Signal&&) = delete;
    Signal& operator=(Signal&&) = delete;

    Subscription connect(Handler handler);
    void emit(Args... args) noexcept;
    void post(Executor& executor, std::decay_t<Args>... args);
    bool hasConnectedHandlers() const;
    void disconnectAll();

  private:
    struct Slot final
    {
      std::size_t id = 0;
      Handler handler;
      bool connected = true;
    };

    class State final
    {
    public:
      std::size_t connect(Handler handler)
      {
        ensureOnOwnerThread();

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
        ensureOnOwnerThread();

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
        ensureOnOwnerThread();
        return _active && std::ranges::any_of(_handlers, [](auto const& slot) { return slot.connected; });
      }

      void disconnect(std::size_t id)
      {
        ensureOnOwnerThread();

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
        ensureOnOwnerThread();
        tombstoneHandlers();
        compactIfIdle();
      }

      void close()
      {
        ensureOnOwnerThread();
        _active = false;
        tombstoneHandlers();
        compactIfIdle();
      }

    private:
      void ensureOnOwnerThread(std::source_location location = std::source_location::current()) const
      {
        AO_EXPECTS_AT(location, std::this_thread::get_id() == _ownerThread, "Signal invoked off the owner thread");
      }

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
        std::erase_if(_handlers, [](Slot const& slot) { return !slot.connected; });
        _needsCompact = false;
      }

      std::deque<Slot> _handlers;
      std::size_t _nextId = 1;
      std::size_t _emitDepth = 0;
      bool _needsCompact = false;
      bool _active = true;
      std::thread::id _ownerThread{std::this_thread::get_id()};
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
  Subscription Signal<Args...>::connect(Handler handler)
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
  void Signal<Args...>::emit(Args... args) noexcept
  {
    auto const statePtr = _statePtr;

    try
    {
      statePtr->emit(args...);
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "signal observer");
    }
  }

  template<typename... Args>
  void Signal<Args...>::post(Executor& executor, std::decay_t<Args>... args)
  {
    auto const weakStatePtr = std::weak_ptr<State>{_statePtr};
    executor.defer(
      [weakStatePtr, ... args = std::move(args)] mutable
      {
        if (auto statePtr = weakStatePtr.lock(); statePtr != nullptr)
        {
          try
          {
            statePtr->emit(args...);
          }
          catch (...)
          {
            AO_FATAL_EXCEPTION(std::current_exception(), "posted signal observer");
          }
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
} // namespace ao::async
