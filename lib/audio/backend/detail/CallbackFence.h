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
    static constexpr std::uint32_t kClosed = std::uint32_t{1U} << 31U;
    static constexpr std::uint32_t kInFlightMask = kClosed - 1U;

    std::atomic<std::uint32_t> _state{kClosed};
  };
} // namespace ao::audio::backend::detail
