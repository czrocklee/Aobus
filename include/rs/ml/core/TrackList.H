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
#include <rs/ml/core/UpdateObserver.H>

namespace rs::ml::core
{
  class TrackList
  {
  public:
    using Value = std::pair<TrackId, const TrackT&>;

    virtual ~TrackList() = default;

    virtual std::size_t size() const = 0;
    virtual Value operator[](std::size_t index) const = 0;
    virtual void mutate(std::size_t index, const TrackT& value) = 0;

    using Observer = UpdateObserver<TrackId, const TrackT&, std::size_t>;
    virtual void attach(Observer& observer) = 0;
    virtual void detach(Observer& observer) = 0;
  };
}

