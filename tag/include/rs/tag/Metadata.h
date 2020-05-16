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

#include <rs/tag/ValueType.h>
#include <map>

namespace rs::tag
{
  enum class MetaField : size_t
  {
    Title = 0,
    Artist,
    Album,
    AlbumArtist,
    AlbumArt,
    Genre,
    Year,
    TrackNumber,
    TotalTracks,
    DiscNumber,
    TotalDiscs,
    LAST_FIELD
  };

  class Metadata
  {
  public:
    const ValueType& get(MetaField field) const
    {
      return _fields[static_cast<std::size_t>(field)];
    }

    void set(MetaField field, ValueType value)
    {
      _fields[static_cast<std::size_t>(field)] = std::move(value);
    }

    const ValueType& getCustom(std::string_view field) const
    {
      if (auto iter = _customFields.find(field); iter != _customFields.end())
      {
        return iter->second;
      }
      else
      {
        static const ValueType DefaultValue{};
        return DefaultValue;
      }
    }

    void setCustom(std::string_view field, ValueType value)
    {
      _customFields.insert_or_assign(std::string{field}, std::move(value));
    }

  private:
    std::array<ValueType, static_cast<std::size_t>(MetaField::LAST_FIELD)> _fields;
    std::map<std::string, ValueType, std::less<>> _customFields;
  };

}
