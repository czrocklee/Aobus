// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/utility/ByteView.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::tag
{
  inline std::string decodeString(std::span<std::byte const> buf)
  {
    return std::string{utility::bytes::stringView(buf)};
  }

  inline std::optional<std::uint16_t> decodeUint16(std::string_view text)
  {
    std::uint16_t result;
    auto const* data = text.data();
    auto [_, ec] = std::from_chars(data, data + text.size(), result);
    return ec == std::errc() ? std::optional{result} : std::nullopt;
  }
} // namespace ao::tag
