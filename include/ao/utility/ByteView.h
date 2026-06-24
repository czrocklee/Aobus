// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ao::utility
{
  namespace detail
  {
    template<typename T>
    constexpr void requireTrivialLayout() noexcept
    {
      static_assert(std::is_trivially_copyable_v<T>, "ByteView helpers require trivially copyable types");
      static_assert(std::is_standard_layout_v<T>, "ByteView helpers require standard-layout types");
    }

    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    inline bool isAligned(void const* ptr, std::size_t alignment) noexcept
    {
      return ptr == nullptr || reinterpret_cast<std::uintptr_t>(ptr) % alignment == 0;
    }
  } // namespace detail

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

    inline std::span<std::byte> view(std::byte* data, std::size_t size) noexcept
    {
      return {data, size};
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

    /**
     * Always-checked typed view over raw bytes. Unlike layout::view<T>, the bounds
     * and alignment checks here are NOT gated on the gsl contract mode (contracts
     * are compiled out in release builds), so this is safe for untrusted /
     * attacker-controlled input. Returns nullptr when the span is shorter than
     * sizeof(T) or misaligned for T.
     */
    template<typename T>
    inline T const* tryLayout(std::span<std::byte const> span) noexcept
    {
      detail::requireTrivialLayout<T>();

      if (span.size() < sizeof(T) || !detail::isAligned(span.data(), alignof(T)))
      {
        return nullptr;
      }

      return reinterpret_cast<T const*>(span.data());
    }

    /**
     * Always-checked typed view that throws std::out_of_range when the span is too
     * short for T or misaligned. Use tryLayout for the non-throwing variant.
     */
    template<typename T>
    inline T const* requireLayout(std::span<std::byte const> span)
    {
      if (auto const* const ptr = tryLayout<T>(span); ptr != nullptr)
      {
        return ptr;
      }

      // NOLINTNEXTLINE(aobus-readability-forbid-raw-throw)
      throw std::out_of_range{"ByteView requireLayout: span too small or misaligned for target type"};
    }
  } // namespace bytes

  namespace layout
  {
    template<typename T>
    inline constexpr bool kIsBinaryLayoutType =
      std::is_trivially_copyable_v<T> && std::is_trivially_default_constructible_v<T> && std::is_standard_layout_v<T>;

    template<typename T>
    inline T const* view(std::span<std::byte const> span) noexcept
    {
      detail::requireTrivialLayout<T>();
      gsl_Expects(span.size() >= sizeof(T));
      gsl_Expects(detail::isAligned(span.data(), alignof(T)));
      return reinterpret_cast<T const*>(span.data());
    }

    template<typename T>
    inline T* viewMutable(std::span<std::byte> span) noexcept
    {
      detail::requireTrivialLayout<T>();
      gsl_Expects(span.size() >= sizeof(T));
      gsl_Expects(detail::isAligned(span.data(), alignof(T)));
      return reinterpret_cast<T*>(span.data());
    }

    template<typename T>
    inline T const* asPtr(std::span<std::byte const> span) noexcept
    {
      detail::requireTrivialLayout<T>();
      gsl_Expects(span.size() >= sizeof(T));
      gsl_Expects(detail::isAligned(span.data(), alignof(T)));
      return reinterpret_cast<T const*>(span.data());
    }

    /**
     * Adapt a const span for legacy C APIs that take non-const pointers but don't mutate.
     * Uses const_cast internally. Use with caution.
     */
    template<typename T>
    inline T* asLegacyPtr(std::span<std::byte const> span) noexcept
    {
      return const_cast<T*>(asPtr<T>(span)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }

    /**
     * Adapt a const pointer for legacy C APIs that take non-const pointers.
     */
    template<typename T, typename U>
    inline T* asLegacyPtr(U const* ptr) noexcept
    {
      return const_cast<T*>(reinterpret_cast<T const*>(ptr)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }

    /**
     * Shortcut for static_cast<std::uint32_t>(span.size()) commonly used in C APIs.
     */
    inline std::uint32_t size32(std::span<std::byte const> span) noexcept
    {
      return static_cast<std::uint32_t>(span.size());
    }

    template<typename T>
    inline T* asMutablePtr(std::span<std::byte> span) noexcept
    {
      detail::requireTrivialLayout<T>();
      gsl_Expects(span.size() >= sizeof(T));
      gsl_Expects(detail::isAligned(span.data(), alignof(T)));
      return reinterpret_cast<T*>(span.data());
    }

    template<typename T>
    inline std::span<T const> viewArray(std::span<std::byte const> span) noexcept
    {
      detail::requireTrivialLayout<T>();
      gsl_Expects(span.size() % sizeof(T) == 0);
      gsl_Expects(detail::isAligned(span.data(), alignof(T)));
      return {reinterpret_cast<T const*>(span.data()), span.size() / sizeof(T)};
    }

    template<typename T>
    inline std::span<T> viewArrayMutable(std::span<std::byte> span) noexcept
    {
      detail::requireTrivialLayout<T>();
      gsl_Expects(span.size() % sizeof(T) == 0);
      gsl_Expects(detail::isAligned(span.data(), alignof(T)));
      return {reinterpret_cast<T*>(span.data()), span.size() / sizeof(T)};
    }

    template<typename T, typename Base>
    inline T const* viewAt(Base const* base, std::size_t offset) noexcept
    {
      detail::requireTrivialLayout<T>();
      auto const* const data = reinterpret_cast<std::byte const*>(base) + offset;
      gsl_Expects(detail::isAligned(data, alignof(T)));
      return reinterpret_cast<T const*>(data);
    }
  } // namespace layout
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

  /**
   * Split a uint64_t into two uint32_t parts for storage.
   * Use when 8-byte alignment is not available.
   */
  namespace uint64Parts
  {
    static constexpr std::uint32_t kMask32 = 0xFFFFFFFF;

    constexpr std::pair<std::uint32_t, std::uint32_t> split(std::uint64_t value) noexcept
    {
      return {static_cast<std::uint32_t>(value & kMask32), static_cast<std::uint32_t>(value >> 32)};
    }

    /**
     * Combine two uint32_t parts back into a uint64_t.
     */
    constexpr std::uint64_t combine(std::uint32_t lo, std::uint32_t hi) noexcept
    {
      return (static_cast<std::uint64_t>(hi) << 32) | lo;
    }
  } // namespace uint64Parts

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
} // namespace ao::utility
