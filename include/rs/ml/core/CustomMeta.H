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

#include <rs/ml/core/Track.H>
#include <rs/ml/core/DataValue.H>

namespace rs::ml::core
{
  inline DataValue customMeta(const Track* track, const char* fieldName)
  {
    auto entry = track->custom()->LookupByKey(fieldName);

    if (entry == nullptr)
    {
      return {};
    }

    switch (entry->value_type())
    {
      case Value_NONE: return Tag{};
      case Value_bool_: return static_cast<const Bool*>(entry->value())->value();
      case Value_int_: return static_cast<const Int*>(entry->value())->value();
      case Value_str:
      {
        auto val = static_cast<const ::flatbuffers::String*>(entry->value());
        return std::string_view{val->c_str(), val->size()};
      }
      default: return {};
    }
  }

  inline DataValue customMeta(const TrackT& track, const char* fieldName)
  {
    auto iter = std::lower_bound(track.custom.begin(), track.custom.end(), fieldName, 
      [](auto& i, const char* v) { return i->key < v; });

    if (iter == track.custom.end())
    {
      return {};
    }

    auto& value = (*iter)->value;

    switch (value.type)
    {
      case Value_NONE: return Tag{};
      case Value_bool_: return static_cast<const Bool*>(value.value)->value();
      case Value_int_: return static_cast<const Int*>(value.value)->value();
      case Value_str: return std::string_view{*static_cast<const std::string*>(value.value)};
      default: return {};
    }
  }
}
