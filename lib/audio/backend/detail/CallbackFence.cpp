// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/CallbackFence.h"

#include <atomic>
#include <thread>

namespace ao::audio::backend::detail
{
  bool CallbackFence::tryEnter() noexcept
  {
    if (!_open.load(std::memory_order_seq_cst))
    {
      return false;
    }

    _inFlight.fetch_add(1U, std::memory_order_seq_cst);
    if (_open.load(std::memory_order_seq_cst))
    {
      return true;
    }

    leave();
    return false;
  }

  void CallbackFence::leave() noexcept
  {
    _inFlight.fetch_sub(1U, std::memory_order_release);
  }

  void CallbackFence::open() noexcept
  {
    _open.store(true, std::memory_order_release);
  }

  void CallbackFence::close() noexcept
  {
    // This store and the admission-side increment/load form a two-atomic
    // handshake. Sequential consistency forbids both sides from observing the
    // other's old value and lets wait() prove that no callback remains live.
    _open.store(false, std::memory_order_seq_cst);
  }

  void CallbackFence::wait() const noexcept
  {
    while (_inFlight.load(std::memory_order_seq_cst) != 0U)
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
    return _open.load(std::memory_order_acquire);
  }
} // namespace ao::audio::backend::detail
