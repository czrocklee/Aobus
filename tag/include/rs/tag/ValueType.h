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

#include <variant>
#include <string>
#include <vector>

namespace rs::tag
{
  using Blob = std::vector<char>;
  using ValueType = std::variant<std::monostate, std::string, std::uint64_t, std::int64_t, double, Blob>;

  inline bool isNull(const ValueType& value)
  {
    return value == ValueType{};
  }
}

