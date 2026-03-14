/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace test
{

  /**
   * Serialize a POD struct to a byte vector.
   */
  template<typename T>
  std::vector<char> serializeHeader(const T& header)
  {
    static_assert(std::is_trivially_copyable_v<T>, "Header must be trivially copyable");

    std::vector<char> data;
    data.insert(data.end(), reinterpret_cast<const char*>(&header), reinterpret_cast<const char*>(&header + 1));
    return data;
  }

  /**
   * Append a null-terminated string to the payload.
   */
  inline void appendString(std::vector<char>& payload, std::string_view str)
  {
    payload.insert(payload.end(), str.begin(), str.end());
    payload.push_back('\0');
  }

} // namespace test
