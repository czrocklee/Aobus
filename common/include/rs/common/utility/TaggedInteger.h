/*
 * Copyright (C) <year> <name of author>
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

namespace rs::common::utility
{
  template<typename T, typename Tag, T Default = T{}>
  class TaggedInteger
  {
  public:
    TaggedInteger() : _value{Default} {}
    explicit TaggedInteger(T value) : _value(value) {}
    operator T() const { return _value; }
    static TaggedInteger invalid() { return {}; }

    friend bool operator<(TaggedInteger a, TaggedInteger b) { return a._value < b._value; }
    friend bool operator==(TaggedInteger a, TaggedInteger b) { return a._value == b._value; }
    friend bool operator!=(TaggedInteger a, TaggedInteger b) { return a._value != b._value; }

  private:
    T _value;
  };

  template<typename Tag>
  using TaggedIndex = TaggedInteger<std::size_t, Tag, std::numeric_limits<std::size_t>::max()>;
}
