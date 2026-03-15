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

#include <rs/core/ListLayout.h>

namespace rs::core
{

  std::string_view ListView::getString(std::uint16_t offset, std::uint16_t len) const
  {
    if (len == 0) return {};

    std::size_t const start = sizeof(ListHeader) + offset;
    if (start + len > _size)
    {
      return {};
    }

    return {reinterpret_cast<char const*>(_payloadBase + start), len};
  }

} // namespace rs::core
