// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ao::utility
{
  template<typename T, typename Tag>
  class StrongType
  {
  public:
    constexpr StrongType() = default;

    constexpr explicit StrongType(T value)
      : _value{std::move(value)}
    {
    }

    constexpr explicit StrongType(char const* value)
      requires std::constructible_from<T, char const*>
      : _value{value}
    {
    }

    constexpr T const& raw() const noexcept { return _value; }

    bool empty() const noexcept
      requires requires(T const& typeVal) { typeVal.empty(); }
    {
      return _value.empty();
    }

    operator std::string_view() const noexcept
      requires std::convertible_to<T const&, std::string_view>
    {
      return _value;
    }

    explicit operator T() const noexcept
      requires std::is_integral_v<T>
    {
      return _value;
    }

    auto operator<=>(StrongType const&) const = default;
    bool operator==(StrongType const&) const = default;

    bool operator==(std::string_view rhs) const noexcept
      requires std::convertible_to<T const&, std::string_view>
    {
      return _value == rhs;
    }

    auto operator<=>(std::string_view rhs) const noexcept
      requires std::convertible_to<T const&, std::string_view>
    {
      return std::string_view{_value} <=> rhs;
    }

    bool operator==(T const& rhs) const noexcept
      requires std::is_integral_v<T>
    {
      return _value == rhs;
    }

    auto operator<=>(T const& rhs) const noexcept
      requires std::is_integral_v<T>
    {
      return _value <=> rhs;
    }

  private:
    T _value{};
  };

  template<typename T, typename Tag>
  inline constexpr bool kIsStrongTypeTriviallyCopyableV = std::is_trivially_copyable_v<StrongType<T, Tag>>;
} // namespace ao::utility

namespace std
{
  template<typename T, typename Tag>
  // NOLINTNEXTLINE(bugprone-std-namespace-modification) -- permitted user-type specialization
  struct hash<ao::utility::StrongType<T, Tag>>
  {
    size_t operator()(ao::utility::StrongType<T, Tag> const& id) const noexcept { return hash<T>{}(id.raw()); }
  };
} // namespace std
