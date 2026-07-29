// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <unordered_set>

namespace ao::uimodel
{
  /**
   * @brief Accepts one key press per physical-key down/up cycle.
   *
   * Native frontends use this for commands where OS auto-repeat would turn one
   * deliberate gesture into several mutations. Key identity is the platform's
   * physical keycode, not the translated symbol, so modifier changes while the
   * key is held do not accidentally admit a repeat.
   */
  class KeyRepeatGuard final
  {
  public:
    bool acceptPress(std::uint32_t physicalKeycode);
    void release(std::uint32_t physicalKeycode);
    void reset() noexcept;

  private:
    std::unordered_set<std::uint32_t> _pressedKeycodes;
  };
} // namespace ao::uimodel
