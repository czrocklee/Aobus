// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace ao::utility
{
  /// 128-bit content hash value, stored and compared as canonical bytes.
  /// Producers define the byte order; persisted signatures use the producing
  /// algorithm's canonical (big-endian) serialization.
  struct Hash128 final
  {
    std::array<std::byte, 16> bytes{};

    constexpr bool operator==(Hash128 const&) const noexcept = default;
  };

  std::string hash128Hex(Hash128 const& hash);
} // namespace ao::utility
