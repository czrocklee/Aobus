// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "Hex.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ao::utility::detail
{
  void appendLowerHex(std::string& text, std::span<std::byte const> bytes)
  {
    constexpr std::uint32_t kNibbleMask = 0xf;

    for (auto const byte : bytes)
    {
      auto const value = std::to_integer<std::uint32_t>(byte);
      text.push_back(kLowerHexDigits[value >> kNibbleBits]);
      text.push_back(kLowerHexDigits[value & kNibbleMask]);
    }
  }

  std::string lowerHex(std::span<std::byte const> bytes)
  {
    auto text = std::string{};
    text.reserve(bytes.size() * 2);
    appendLowerHex(text, bytes);
    return text;
  }
} // namespace ao::utility::detail
