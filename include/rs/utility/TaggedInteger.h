/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

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
    TaggedInteger() : _value{Default} {}
    explicit TaggedInteger(T value) : _value(value) {}
    [[nodiscard]] T value() const { return _value; }
    explicit operator T() const { return _value; }
    [[nodiscard]] static TaggedInteger invalid() { return {}; }

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

    friend bool operator<(TaggedInteger a, TaggedInteger b) { return a._value < b._value; }
    friend bool operator==(TaggedInteger a, TaggedInteger b) { return a._value == b._value; }
    friend bool operator!=(TaggedInteger a, TaggedInteger b) { return a._value != b._value; }

    // Comparison with underlying type
    friend bool operator<(TaggedInteger a, T b) { return a._value < b; }
    friend bool operator<(T a, TaggedInteger b) { return a < b._value; }
    friend bool operator==(TaggedInteger a, T b) { return a._value == b; }
    friend bool operator==(T a, TaggedInteger b) { return a == b._value; }
    friend bool operator!=(TaggedInteger a, T b) { return a._value != b; }
    friend bool operator!=(T a, TaggedInteger b) { return a != b._value; }
    friend bool operator>(TaggedInteger a, T b) { return a._value > b; }
    friend bool operator>(T a, TaggedInteger b) { return a > b._value; }
    friend bool operator<=(TaggedInteger a, T b) { return a._value <= b; }
    friend bool operator>=(TaggedInteger a, T b) { return a._value >= b; }

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
