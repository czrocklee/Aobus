// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

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
    return std::string{reinterpret_cast<char const*>(buf.data()), buf.size()};
  }

  inline std::optional<std::uint16_t> decodeUint16(std::span<std::byte const> buf)
  {
    char const* data = reinterpret_cast<char const*>(buf.data());
    std::uint16_t result;
    auto [_, ec] = std::from_chars(data, data + buf.size(), result);
    return ec == std::errc() ? std::optional{result} : std::nullopt;
  }

  inline std::span<std::byte const> viewBytes(std::span<std::byte const> buf)
  {
    return buf;
  }
} // namespace rs::tag
