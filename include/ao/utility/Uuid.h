// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace ao::utility
{
  constexpr std::size_t kUuidByteCount = 16;

  using UuidBytes = std::array<std::byte, kUuidByteCount>;

  std::string formatUuid(UuidBytes const& id);
} // namespace ao::utility
