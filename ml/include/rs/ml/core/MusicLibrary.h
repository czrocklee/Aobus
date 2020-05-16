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

#include <rs/ml/fbs/Track_generated.h>
#include <rs/ml/fbs/List_generated.h>
#include <rs/ml/core/FlatBuffersStore.h>
#include <rs/ml/core/ResourceStore.h>
#include <rs/ml/core/LMDBTransaction.h>

namespace rs::ml::core
{
  class MusicLibrary 
  {
  public:
    using TrackStore = FlatBuffersStore<fbs::Track>;
    using ListStore = FlatBuffersStore<fbs::List>;
    using TrackId = typename TrackStore::Id;
    using ListId = typename ListStore::Id;

    explicit MusicLibrary(const std::string& rootDir);

    LMDBReadTransaction readTransaction() const { return LMDBReadTransaction{_env}; }
    LMDBWriteTransaction writeTransaction() { return LMDBWriteTransaction{_env}; }

    TrackStore& tracks() { return _tracks; }
    const TrackStore& tracks() const { return _tracks; }

    ListStore& lists() { return _lists; }
    const ListStore& lists() const { return _lists; }

    ResourceStore& resources() { return _resources; }
    const ResourceStore& resources() const { return _resources; }

  private:
    lmdb::env _env;
    TrackStore _tracks;
    ListStore _lists;
    ResourceStore _resources;
  };
}
