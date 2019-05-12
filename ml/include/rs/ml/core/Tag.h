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

#include <rs/ml/core/Track.h>
#include <rs/ml/core/DataValue.h>

namespace rs::ml::core
{
  inline DataValue tag(const fbs::Track* track, const char* tagName)
  {
    auto tags = track->tags();
    auto iter = std::find_if(tags->begin(), tags->end(),
      [tagName](auto t) { return ::strcmp(t->c_str(), tagName) == 0; });
    return iter == tags->end() ? DataValue{false} : DataValue{true};
  }

  inline DataValue tag(const fbs::TrackT& track, const char* tagName)
  {
    auto iter = std::find_if(track.tags.begin(), track.tags.end(),
      [tagName](const auto& t) { return t == tagName; });
    return iter == track.tags.end() ? DataValue{false} : DataValue{true};
  }
}
