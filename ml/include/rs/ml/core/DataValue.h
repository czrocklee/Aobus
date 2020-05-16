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

#include <boost/container/small_vector.hpp>
#include <boost/variant.hpp>
#include <variant>

#include <vector>
#include <string>

namespace rs::ml::core
{
//  struct DataValueVector;

//  using DataValueVectorPtr = std::unique_ptr<DataValueVector, void(*)(DataValueVector*)>;

  //using DataValue = std::variant<boost::blank, bool, std::int64_t, std::string_view, DataValueVectorPtr>;

//  struct Tag { inline bool operator==(Tag) const { return false; } };
  using DataValue = std::variant<std::monostate, bool, std::int64_t, std::string_view, std::string>;

//  struct DataValueVector : public boost::container::small_vector<DataValue, 4> { };
}
