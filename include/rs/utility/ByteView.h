// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace rs::utility
{
  // Cast span<byte> to pointer of type T (for deserialization)
  template<typename T>
  constexpr T const* as(std::span<std::byte const> span) noexcept
  {
    return reinterpret_cast<T const*>(span.data());
  }

  // Cast span<byte> to span<T> (array of T)
  template<typename T>
  constexpr std::span<T const> asArray(std::span<std::byte const> span) noexcept
  {
    return {reinterpret_cast<T const*>(span.data()), span.size() / sizeof(T)};
  }

  // Cast typed pointer with offset (e.g., uint8_t* + offset -> T const*)
  // Binary deserialization utility - offset-based pointer arithmetic is unavoidable here
  template<typename T, typename Base>
  constexpr T const* as(Base const* base, std::size_t offset) noexcept
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<T const*>(reinterpret_cast<std::uint8_t const*>(base) + offset);
  }

  // Cast pointer to span<byte> (for serialization)
  template<typename T>
  constexpr std::span<std::byte const> asBytes(T const* ptr, std::size_t count = 1) noexcept
  {
    return {reinterpret_cast<std::byte const*>(ptr), count * sizeof(T)};
  }

  // Get bytes span from a single object
  template<typename T>
  constexpr std::span<std::byte const> asBytes(T const& obj) noexcept
  {
    return asBytes(std::addressof(obj));
  }

  // Get bytes span from string_view
  inline std::span<std::byte const> asBytes(std::string_view str) noexcept
  {
    return asBytes(str.data(), str.size());
  }

  // Get bytes span from std::string
  inline std::span<std::byte const> asBytes(std::string const& str) noexcept
  {
    return asBytes(str.data(), str.size());
  }

  // Get string_view from span<byte>
  inline std::string_view asString(std::span<std::byte const> span) noexcept
  {
    return {reinterpret_cast<char const*>(span.data()), span.size()};
  }

  // Get string_view from byte pointer + offset + size
  inline std::string_view asString(std::byte const* data, std::size_t offset, std::size_t size) noexcept
  {
    return {reinterpret_cast<char const*>(data) + offset, size};
  }

  /**
   * Split a uint64_t into two uint32_t parts for storage.
   * Use when 8-byte alignment is not available.
   */
  constexpr std::pair<std::uint32_t, std::uint32_t> splitInt64(std::uint64_t value) noexcept
  {
    return {static_cast<std::uint32_t>(value & 0xFFFFFFFF),
            static_cast<std::uint32_t>(value >> 32)};
  }

  /**
   * Combine two uint32_t parts back into a uint64_t.
   */
  constexpr std::uint64_t combineInt64(std::uint32_t lo, std::uint32_t hi) noexcept
  {
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
  }
}
