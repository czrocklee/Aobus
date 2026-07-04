// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace ao::utility
{
  struct Hash128 final
  {
    std::array<std::byte, 16> bytes{};

    constexpr bool operator==(Hash128 const&) const noexcept = default;
  };

  inline constexpr std::uint32_t kFnv32OffsetBasis = 2166136261U;
  inline constexpr std::uint32_t kFnv32Prime = 16777619U;

  inline constexpr std::uint64_t kFnv64OffsetBasis = 14695981039346656037ULL;
  inline constexpr std::uint64_t kFnv64Prime = 1099511628211ULL;

  namespace detail
  {
    struct UInt128 final
    {
      std::uint64_t high = 0;
      std::uint64_t low = 0;
    };

    constexpr UInt128 makeUInt128(std::uint64_t high, std::uint64_t low) noexcept
    {
      return {.high = high, .low = low};
    }

    constexpr UInt128 kFnv128OffsetBasis = makeUInt128(0x6c62272e07bb0142ULL, 0x62b821756295c58dULL);
    constexpr std::uint16_t kFnv128PrimeLow = 0x013BU;
    constexpr std::uint64_t kLowByteMask = 0xFFULL;
    constexpr std::uint32_t kFnv128PrimeShift = 88U;
    constexpr std::uint32_t kUInt128Bits = 128U;
    constexpr std::uint32_t kUInt64Bits = 64U;

    constexpr UInt128 addUInt128(UInt128 left, UInt128 right) noexcept
    {
      auto result = UInt128{.high = left.high + right.high, .low = left.low + right.low};

      if (result.low < left.low)
      {
        ++result.high;
      }

      return result;
    }

    constexpr UInt128 shiftLeftUInt128(UInt128 value, std::uint32_t shift) noexcept
    {
      if (shift == 0U)
      {
        return value;
      }

      if (shift >= kUInt128Bits)
      {
        return {};
      }

      if (shift >= kUInt64Bits)
      {
        return {.high = value.low << (shift - kUInt64Bits), .low = 0};
      }

      return {.high = (value.high << shift) | (value.low >> (kUInt64Bits - shift)), .low = value.low << shift};
    }

    constexpr UInt128 multiplyUInt128BySmall(UInt128 value, std::uint16_t factor) noexcept
    {
      auto result = UInt128{};

      while (factor != 0)
      {
        if ((factor & 1U) != 0)
        {
          result = addUInt128(result, value);
        }

        factor >>= 1U;
        value = shiftLeftUInt128(value, 1U);
      }

      return result;
    }

    constexpr UInt128 multiplyFnv128Prime(UInt128 value) noexcept
    {
      return addUInt128(shiftLeftUInt128(value, kFnv128PrimeShift), multiplyUInt128BySmall(value, kFnv128PrimeLow));
    }

    constexpr void mixFnv128Byte(UInt128& hash, std::byte byte) noexcept
    {
      hash.low ^= std::to_integer<std::uint8_t>(byte);
      hash = multiplyFnv128Prime(hash);
    }

    constexpr Hash128 toHash128(UInt128 value) noexcept
    {
      auto hash = Hash128{};

      for (std::size_t index = 0; index < 8U; ++index)
      {
        auto const highShift = static_cast<std::uint32_t>((7U - index) * 8U);
        hash.bytes[index] = static_cast<std::byte>((value.high >> highShift) & kLowByteMask);
        hash.bytes[index + 8U] = static_cast<std::byte>((value.low >> highShift) & kLowByteMask);
      }

      return hash;
    }
  } // namespace detail

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

  /// 128-bit FNV-1a over a byte range, suitable for non-security content signatures.
  constexpr Hash128 fnv1a128(std::span<std::byte const> data) noexcept
  {
    auto hash = detail::kFnv128OffsetBasis;

    for (std::byte const byte : data)
    {
      detail::mixFnv128Byte(hash, byte);
    }

    return detail::toHash128(hash);
  }

  constexpr Hash128 fnv1a128(std::string_view text) noexcept
  {
    auto hash = detail::kFnv128OffsetBasis;

    for (char const ch : text)
    {
      detail::mixFnv128Byte(hash, static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }

    return detail::toHash128(hash);
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

  inline std::string hash128Hex(Hash128 const& hash)
  {
    auto result = std::string{};
    result.reserve(hash.bytes.size() * 2U);

    for (std::byte const byte : hash.bytes)
    {
      result += std::format("{:02x}", static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(byte)));
    }

    return result;
  }

  inline std::string fnv1a128Hex(std::span<std::byte const> data)
  {
    return hash128Hex(fnv1a128(data));
  }

  inline std::string fnv1a128Hex(std::string_view text)
  {
    return hash128Hex(fnv1a128(text));
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

  /// Incremental 128-bit FNV-1a for streamed non-security content signatures.
  class Fnv1a128Accumulator final
  {
  public:
    void mix(std::span<std::byte const> bytes) noexcept
    {
      for (std::byte const byte : bytes)
      {
        detail::mixFnv128Byte(_hash, byte);
      }
    }

    void mix(std::string_view bytes) noexcept
    {
      for (char const ch : bytes)
      {
        detail::mixFnv128Byte(_hash, static_cast<std::byte>(static_cast<unsigned char>(ch)));
      }
    }

    Hash128 value() const noexcept { return detail::toHash128(_hash); }

    std::string hex() const { return hash128Hex(value()); }

  private:
    detail::UInt128 _hash = detail::kFnv128OffsetBasis;
  };
} // namespace ao::utility
