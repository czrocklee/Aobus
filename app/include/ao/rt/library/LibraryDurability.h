// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::rt
{
  /// Whether music-library commits wait for stable storage before they return.
  enum class LibraryDurability : std::uint8_t
  {
    /// A returned commit survives an operating-system crash or power loss.
    Durable,
    /// A returned commit survives a process crash, but an operating-system
    /// crash or power loss can lose the most recent commits or corrupt the
    /// library. Only for disposable libraries, such as test fixtures.
    Unflushed,
  };
} // namespace ao::rt
