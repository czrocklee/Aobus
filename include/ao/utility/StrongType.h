// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <concepts>
#include <cstddef>
#include <format>
#include <functional>
#include <ostream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ao::utility
{
  template<typename T, typename Tag>
  class StrongType
  {
  public:
    StrongType() = default;

    explicit StrongType(T value)
      : _value{std::move(value)}
    {
    }

    explicit StrongType(char const* value)
      requires std::constructible_from<T, char const*>
      : _value{value}
    {
    }

    T const& value() const noexcept { return _value; }
    T& value() noexcept { return _value; }

    bool empty() const noexcept
      requires requires(T const& typeVal) { typeVal.empty(); }
    {
      return _value.empty();
    }

    void clear() noexcept
      requires requires(T& typeVal) { typeVal.clear(); }
    {
      _value.clear();
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

    StrongType& operator++()
      requires std::is_integral_v<T>
    {
      ++_value;
      return *this;
    }

    StrongType operator++(int)
      requires std::is_integral_v<T>
    {
      auto tmp = *this;
      ++_value;
      return tmp;
    }

    StrongType& operator--()
      requires std::is_integral_v<T>
    {
      --_value;
      return *this;
    }

    StrongType operator--(int)
      requires std::is_integral_v<T>
    {
      auto tmp = *this;
      --_value;
      return tmp;
    }

    friend std::ostream& operator<<(std::ostream& os, StrongType const& val)
      requires std::is_integral_v<T>
    {
      return os << val._value;
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
  struct hash<ao::utility::StrongType<T, Tag>>
  {
    size_t operator()(ao::utility::StrongType<T, Tag> const& id) const noexcept { return hash<T>{}(id.value()); }
  };

  template<typename T, typename Tag>
  struct formatter<ao::utility::StrongType<T, Tag>> : formatter<T>
  {
    auto format(ao::utility::StrongType<T, Tag> const& id, format_context& ctx) const
    {
      return formatter<T>::format(id.value(), ctx);
    }
  };
} // namespace std
