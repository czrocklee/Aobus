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

#include <rs/lmdb/Environment.h>

#include <cstring>
#include <stdexcept>
#include <string_view>

namespace rs::lmdb
{
  // Error handling

  inline void throwOnError(const char* origin, int code)
  {
    if (code != MDB_SUCCESS)
    {
      throw std::runtime_error{std::string{origin} + ": " + mdb_strerror(code)};
    }
  }

  // Helper functions

  template<typename T>
  [[nodiscard]] std::string_view bytesOf(const T& value)
  {
    return {reinterpret_cast<const char*>(std::addressof(value)), sizeof(value)};
  }

  template<typename T>
  [[nodiscard]] std::string_view bytesOf(const T* value)
  {
    return {reinterpret_cast<const char*>(value), sizeof(T)};
  }

  template<typename T>
  [[nodiscard]] T read(std::string_view bytes)
  {
    if (bytes.size() != sizeof(T))
    {
      throw std::runtime_error{"lmdb::read: bad value size"};
    }
    T value;
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
  }
}
