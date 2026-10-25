// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/CallbackFence.h"

#include <ao/Contract.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace ao::audio::backend::detail
{
  bool CallbackFence::tryEnter() noexcept
  {
    auto observed = _state.load(std::memory_order_relaxed);

    while ((observed & kClosed) == 0U)
    {
      if (_state.compare_exchange_weak(observed, observed + 1U, std::memory_order_acquire, std::memory_order_relaxed))
      {
        return true;
      }
    }

    // Rejected native callbacks do not borrow or mutate the closed fence.
    // In particular, they cannot race reopen with a transient in-flight count.
    return false;
  }

  void CallbackFence::leave() noexcept
  {
    _state.fetch_sub(1U, std::memory_order_release);
  }

  void CallbackFence::open() noexcept
  {
    std::uint32_t expected = kClosed;
    auto const opened =
      _state.compare_exchange_strong(expected, 0U, std::memory_order_release, std::memory_order_relaxed);
    AO_INVARIANT(opened, "Cannot reopen a callback fence before its callbacks quiesce");
  }

  void CallbackFence::close() noexcept
  {
    _state.fetch_or(kClosed, std::memory_order_acq_rel);
  }

  void CallbackFence::wait() const noexcept
  {
    while ((_state.load(std::memory_order_acquire) & kInFlightMask) != 0U)
    {
      std::this_thread::yield();
    }
  }

  void CallbackFence::closeAndWait() noexcept
  {
    close();
    wait();
  }

  bool CallbackFence::isOpen() const noexcept
  {
    return (_state.load(std::memory_order_acquire) & kClosed) == 0U;
  }
} // namespace ao::audio::backend::detail
