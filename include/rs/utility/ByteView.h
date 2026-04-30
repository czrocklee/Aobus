// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gsl-lite/gsl-lite.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace rs::utility
{
  namespace detail
  {
    template<typename T>
    constexpr void requireTrivialLayout() noexcept
    {
      static_assert(std::is_trivially_copyable_v<T>, "ByteView helpers require trivially copyable types");
      static_assert(std::is_standard_layout_v<T>, "ByteView helpers require standard-layout types");
    }

    inline bool isAligned(void const* ptr, std::size_t alignment) noexcept
    {
      return ptr == nullptr || reinterpret_cast<std::uintptr_t>(ptr) % alignment == 0;
    }
  }

  namespace bytes
  {
    inline std::span<std::byte const> view(void const* data, std::size_t size) noexcept
    {
      return {static_cast<std::byte const*>(data), size};
    }

    inline std::span<std::byte> view(void* data, std::size_t size) noexcept
    {
      return {static_cast<std::byte*>(data), size};
    }

    template<typename T>
    inline std::span<std::byte const> view(T const* ptr, std::size_t count = 1) noexcept
    {
      detail::requireTrivialLayout<T>();
      return {reinterpret_cast<std::byte const*>(ptr), count * sizeof(T)};
    }

    template<typename T, std::size_t Extent>
    inline std::span<std::byte const> view(std::span<T, Extent> values) noexcept
    {
      detail::requireTrivialLayout<std::remove_const_t<T>>();
      return {reinterpret_cast<std::byte const*>(values.data()), values.size_bytes()};
    }

    template<typename T>
    inline std::span<std::byte const> view(T const& value) noexcept
    {
      detail::requireTrivialLayout<T>();
      return view(std::addressof(value));
    }

    inline std::span<std::byte const> view(std::string_view str) noexcept
    {
      return view(str.data(), str.size());
    }

    inline std::span<std::byte const> view(std::string const& str) noexcept
    {
      return view(str.data(), str.size());
    }

    inline std::string_view stringView(std::span<std::byte const> span) noexcept
    {
      if (span.empty())
      {
        return {};
      }

      return {reinterpret_cast<char const*>(span.data()), span.size()};
    }
  }

  namespace layout
  {
    template<typename T>
    inline T const* view(std::span<std::byte const> span) noexcept
    {
      detail::requireTrivialLayout<T>();
      gsl_Expects(span.size() >= sizeof(T));
      gsl_Expects(detail::isAligned(span.data(), alignof(T)));
      return reinterpret_cast<T const*>(span.data());
    }

    template<typename T>
    inline std::span<T const> viewArray(std::span<std::byte const> span) noexcept
    {
      detail::requireTrivialLayout<T>();
      gsl_Expects(span.size() % sizeof(T) == 0);
      gsl_Expects(detail::isAligned(span.data(), alignof(T)));
      return {reinterpret_cast<T const*>(span.data()), span.size() / sizeof(T)};
    }

    template<typename T, typename Base>
    inline T const* viewAt(Base const* base, std::size_t offset) noexcept
    {
      detail::requireTrivialLayout<T>();
      auto const* data = reinterpret_cast<std::byte const*>(base) + offset;
      gsl_Expects(detail::isAligned(data, alignof(T)));
      return reinterpret_cast<T const*>(data);
    }
  }

  /**
   * Split a uint64_t into two uint32_t parts for storage.
   * Use when 8-byte alignment is not available.
   */
  namespace uint64Parts
  {
    constexpr std::pair<std::uint32_t, std::uint32_t> split(std::uint64_t value) noexcept
    {
      return {static_cast<std::uint32_t>(value & 0xFFFFFFFF), static_cast<std::uint32_t>(value >> 32)};
    }

    /**
     * Combine two uint32_t parts back into a uint64_t.
     */
    constexpr std::uint64_t combine(std::uint32_t lo, std::uint32_t hi) noexcept
    {
      return (static_cast<std::uint64_t>(hi) << 32) | lo;
    }
  }

  /**
   * Explicitly unchecked downcast. Use when the derived type is guaranteed
   * by the application logic and performance is preferred over RTTI overhead.
   */
  template<typename T, typename U>
  inline T* unsafeDowncast(U* ptr) noexcept
  {
    static_assert(std::is_void_v<U> || std::is_base_of_v<U, T>, "T must be derived from U for unsafeDowncast");
    gsl_Expects(ptr != nullptr);
    return static_cast<T*>(ptr); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
  }

  template<typename T, typename U>
  inline T& unsafeDowncast(U& ref) noexcept
  {
    static_assert(std::is_base_of_v<U, T>, "T must be derived from U for unsafeDowncast");
    return static_cast<T&>(ref); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
  }
}
