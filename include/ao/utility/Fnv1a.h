// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace ao::utility
{
  inline constexpr std::uint32_t kFnv32OffsetBasis = 2166136261U;
  inline constexpr std::uint32_t kFnv32Prime = 16777619U;

  inline constexpr std::uint64_t kFnv64OffsetBasis = 14695981039346656037ULL;
  inline constexpr std::uint64_t kFnv64Prime = 1099511628211ULL;

  /// 32-bit FNV-1a over a byte range. Stable enough for content-addressable keys.
  constexpr std::uint32_t fnv1a32(std::span<std::byte const> data) noexcept
  {
    auto hash = kFnv32OffsetBasis;

    for (std::byte const byte : data)
    {
      hash ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(byte));
      hash *= kFnv32Prime;
    }

    return hash;
  }

  constexpr std::uint64_t fnv1a64(std::string_view text) noexcept
  {
    auto hash = kFnv64OffsetBasis;

    for (char const ch : text)
    {
      hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
      hash *= kFnv64Prime;
    }

    return hash;
  }

  /// Fixed-width 16-hex-digit form, suitable for stable keys and fingerprints.
  inline std::string fnv1a64Hex(std::string_view text)
  {
    return std::format("{:016x}", fnv1a64(text));
  }

  /// Incremental 64-bit FNV-1a, for inputs that arrive in chunks (e.g. streamed files).
  class Fnv1a64Accumulator final
  {
  public:
    void mix(std::string_view bytes) noexcept
    {
      for (char const ch : bytes)
      {
        _hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        _hash *= kFnv64Prime;
      }
    }

    std::uint64_t value() const noexcept { return _hash; }

    std::string hex() const { return std::format("{:016x}", _hash); }

  private:
    std::uint64_t _hash = kFnv64OffsetBasis;
  };
} // namespace ao::utility
