// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstddef>
#include <limits>
#include <ostream>
#include <type_traits>
#include <unordered_map>

namespace rs::utility
{
  template<typename T, typename Tag, T Default = T{}>
  class TaggedInteger
  {
  public:
    TaggedInteger()
      : _value{Default}
    {
    }
    explicit TaggedInteger(T value)
      : _value{value}
    {
    }
    T value() const { return _value; }
    explicit operator T() const { return _value; }
    static TaggedInteger invalid() { return {}; }

    // Increment/decrement operators
    TaggedInteger& operator++()
    {
      ++_value;
      return *this;
    }
    TaggedInteger operator++(int)
    {
      auto tmp = *this;
      ++_value;
      return tmp;
    }
    TaggedInteger& operator--()
    {
      --_value;
      return *this;
    }
    TaggedInteger operator--(int)
    {
      auto tmp = *this;
      --_value;
      return tmp;
    }

    friend auto operator<=>(TaggedInteger const&, TaggedInteger const&) = default;
    friend bool operator==(TaggedInteger const&, TaggedInteger const&) = default;
    friend auto operator<=>(TaggedInteger const& lhs, T const& rhs) { return lhs._value <=> rhs; }
    friend bool operator==(TaggedInteger const& lhs, T const& rhs) { return lhs._value == rhs; }

    // Stream output
    friend std::ostream& operator<<(std::ostream& os, TaggedInteger val) { return os << val._value; }

  private:
    T _value;
  };

  // Static assert after class definition (requires complete type)
  template<typename T, typename Tag, T Default>
  inline constexpr bool is_tagged_integer_trivially_copyable_v =
    std::is_trivially_copyable_v<TaggedInteger<T, Tag, Default>>;

  template<typename Tag>
  using TaggedIndex = TaggedInteger<std::size_t, Tag, std::numeric_limits<std::size_t>::max()>;
}

namespace std
{
  // Specialization of std::hash for TaggedInteger to enable use in unordered_map
  template<typename T, typename Tag, T Default>
  struct hash<rs::utility::TaggedInteger<T, Tag, Default>>
  {
    std::size_t operator()(rs::utility::TaggedInteger<T, Tag, Default> tagged) const noexcept
    {
      return std::hash<T>{}(tagged.value());
    }
  };
}
