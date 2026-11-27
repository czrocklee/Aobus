// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ao::utility::detail
{
  inline constexpr auto kLowerHexDigits = std::string_view{"0123456789abcdef"};
  inline constexpr std::uint32_t kNibbleBits = 4;

  void appendLowerHex(std::string& text, std::span<std::byte const> bytes);
  std::string lowerHex(std::span<std::byte const> bytes);
} // namespace ao::utility::detail
