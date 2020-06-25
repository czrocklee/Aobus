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

#include <rs/tag/ValueType.h>
#include <charconv>

namespace rs::tag
{
  struct StringDecoder
  {
    static ValueType decode(const void* buffer, std::size_t size)
    {
      return std::string{static_cast<const char*>(buffer), size};
    }
  };

  struct IntDecoder
  {
    static ValueType decode(const void* buffer, std::size_t size)
    {
      const char* data = static_cast<const char*>(buffer);
      std::int64_t result;
      auto [_, ec] = std::from_chars(data, data + size, result);
      return ec == std::errc() ? ValueType{result} : ValueType{};
    }
  };

  struct BlobDecoder
  {
    static ValueType decode(const void* buffer, std::size_t size)
    {
      const char* data = static_cast<const char*>(buffer);
      std::vector<char> blob;
      blob.assign(data, data + size);
      return {std::move(blob)};
    }
  };
}