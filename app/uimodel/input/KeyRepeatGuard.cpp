// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/input/KeyRepeatGuard.h>

#include <cstdint>

namespace ao::uimodel
{
  bool KeyRepeatGuard::acceptPress(std::uint32_t const physicalKeycode)
  {
    return _pressedKeycodes.insert(physicalKeycode).second;
  }

  void KeyRepeatGuard::release(std::uint32_t const physicalKeycode)
  {
    _pressedKeycodes.erase(physicalKeycode);
  }

  void KeyRepeatGuard::reset() noexcept
  {
    _pressedKeycodes.clear();
  }
} // namespace ao::uimodel
