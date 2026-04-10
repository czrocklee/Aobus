// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/utility/ByteView.h>

#include <cstdint>
#include <charconv>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rs::tag
{
  inline std::string decodeString(std::span<std::byte const> buf)
  {
    return std::string{rs::utility::bytes::stringView(buf)};
  }

  inline std::optional<std::uint16_t> decodeUint16(std::string_view text)
  {
    std::uint16_t result;
    auto const* data = text.data();
    auto [_, ec] = std::from_chars(data, data + text.size(), result);
    return ec == std::errc() ? std::optional{result} : std::nullopt;
  }
} // namespace rs::tag
