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

#include <rs/core/TrackLayout.h>

#include <cstring>

namespace rs::core
{

  // TrackView::TagProxy implementation
  std::vector<DictionaryId> TrackView::TagProxy::ids() const
  {
    std::vector<DictionaryId> result;
    auto c = count();
    for (std::uint8_t i = 0; i < c; ++i)
    {
      auto tagId = id(i);
      if (tagId > 0)
      {
        result.push_back(tagId);
      }
    }
    return result;
  }

  bool TrackView::TagProxy::has(DictionaryId tagIdToCheck) const
  {
    auto c = count();
    for (std::uint8_t i = 0; i < c; ++i)
    {
      if (id(i) == tagIdToCheck)
      {
        return true;
      }
    }
    return false;
  }

  // TrackView private implementation
  std::string_view TrackView::getString(std::uint16_t offset, std::uint16_t len) const
  {
    if (len == 0)
      return {};

    const std::size_t start = sizeof(TrackHeader) + offset;
    if (start + len > _size)
    {
      return {};
    }

    return {reinterpret_cast<const char*>(_payloadBase + start), len};
  }

  std::uint32_t TrackView::tagId(std::uint8_t index) const
  {
    if (index >= _header->tagCount)
    {
      return 0;
    }
    auto offset = sizeof(TrackHeader) + _header->tagsOffset + index * sizeof(std::uint32_t);
    if (offset + sizeof(std::uint32_t) > _size)
    {
      return 0;
    }
    return *reinterpret_cast<const std::uint32_t*>(_payloadBase + offset);
  }

} // namespace rs::core
