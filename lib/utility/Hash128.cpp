// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace ao::utility
{
  std::string hash128Hex(Hash128 const& hash)
  {
    auto result = std::string{};
    result.reserve(hash.bytes.size() * 2U);

    for (std::byte const byte : hash.bytes)
    {
      result += std::format("{:02x}", static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(byte)));
    }

    return result;
  }
} // namespace ao::utility
