// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace ao::utility
{
  inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
  inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

  constexpr std::uint64_t fnv1a64(std::string_view text) noexcept
  {
    auto hash = kFnvOffsetBasis;

    for (char const ch : text)
    {
      hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
      hash *= kFnvPrime;
    }

    return hash;
  }

  inline std::string fnv1a64Hex(std::string_view text)
  {
    auto const hash = fnv1a64(text);

    auto buffer = std::array<char, 32>{};
    auto const result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), hash, 16);

    if (result.ec == std::errc{})
    {
      auto const len = static_cast<std::size_t>(result.ptr - buffer.data());
      auto stream = std::ostringstream{};
      stream << std::hex << std::setfill('0') << std::setw(16);
      stream.write(buffer.data(), static_cast<std::streamsize>(len));
      return stream.str();
    }

    auto fallback = std::ostringstream{};
    fallback << std::hex << std::setfill('0') << std::setw(16) << hash;
    return fallback.str();
  }
} // namespace ao::utility
