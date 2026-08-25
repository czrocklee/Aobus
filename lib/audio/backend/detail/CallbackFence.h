// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <atomic>
#include <cstdint>

namespace ao::audio::backend::detail
{
  /** @brief Allocation-free callback admission with a control-side quiescence fence. */
  class CallbackFence final
  {
  public:
    bool tryEnter() noexcept;
    void leave() noexcept;

    void open() noexcept;
    void close() noexcept;
    void wait() const noexcept;
    void closeAndWait() noexcept;
    bool isOpen() const noexcept;

  private:
    std::atomic<bool> _open{false};
    std::atomic<std::uint32_t> _inFlight{0U};
  };
} // namespace ao::audio::backend::detail
