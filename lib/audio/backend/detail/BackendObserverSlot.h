// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/utility/CallbackStackScope.h>

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>

namespace ao::audio::backend::detail
{
  // Closing prevents any new borrow, including a snapshot taken before close.
  // A callback can retire its own registration or registry without waiting for
  // its stack to unwind; an external caller waits for the outstanding calls.
  class BackendObserverCalls final
  {
  public:
    template<typename Invoke>
    void invoke(Invoke&& call)
    {
      {
        auto const lock = std::scoped_lock{_mutex};

        if (_closed)
        {
          return;
        }

        ++_active;
      }

      auto const borrow = Borrow{*this};
      std::forward<Invoke>(call)();
    }

    void retire() noexcept;

  private:
    struct Borrow final
    {
      explicit Borrow(BackendObserverCalls& owner)
        : calls{owner}, scope{&owner}
      {
      }

      Borrow(Borrow const&) = delete;
      Borrow& operator=(Borrow const&) = delete;
      Borrow(Borrow&&) = delete;
      Borrow& operator=(Borrow&&) = delete;

      ~Borrow();

      BackendObserverCalls& calls;
      utility::CallbackStackScope scope;
    };

    std::mutex _mutex;
    std::condition_variable _quiescent;
    std::size_t _active = 0;
    bool _closed = false;
  };

  // Owned by a registration. A snapshot identifies the slot weakly; only the
  // caller of invoke() needs a strong reference through the observer's return.
  template<typename Callback>
  class BackendObserverSlot final
  {
  public:
    explicit BackendObserverSlot(Callback callback)
      : _callback{std::move(callback)}
    {
    }

    template<typename Invoke>
    void invoke(Invoke&& invokeCallback)
    {
      _calls.invoke([&] { std::forward<Invoke>(invokeCallback)(std::as_const(_callback)); });
    }

    void retire() noexcept { _calls.retire(); }

  private:
    Callback _callback;
    BackendObserverCalls _calls;
  };
} // namespace ao::audio::backend::detail
