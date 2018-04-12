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

#include <rs/ml/core/MediaLibrary.H>
#include <rs/ml/core/Protocol_generated.h>

namespace rs::ml::core
{
  MediaLibrary::~MediaLibrary()
  {
    TrackId id = 0;
    _metaDb.put(_txn, id, _nextTrackId);
    _txn.commit();
  }

  MediaLibrary::MediaLibrary(const std::string& folder)
    : _env{nullptr}, _txn{nullptr}, _metaDb{MDB_dbi{}}, _resourceDb{MDB_dbi{}}, _nextTrackId{1}
  {
    _env = lmdb::env::create();
    _env.set_mapsize(1UL * 1024UL * 1024UL * 1024UL);
    _env.set_max_dbs(2);
    _env.open(folder.c_str(), 0, 0664);

    _txn = lmdb::txn::begin(_env);
    _metaDb = lmdb::dbi::open(_txn, "meta", MDB_CREATE | MDB_INTEGERKEY);
    _resourceDb = lmdb::dbi::open(_txn, "resource", MDB_CREATE);

    TrackId id = 0;

    if (_metaDb.get(_txn, id, _nextTrackId))
    {
      _metaDb.del(_txn, id);
    }
  }

  MediaLibrary::Iterator MediaLibrary::begin() const
  {
    return {lmdb::cursor::open(_txn, _metaDb)};
  }

  MediaLibrary::Iterator MediaLibrary::end() const
  {
    return {};
  }

  MediaLibrary::TrackId MediaLibrary::add(const Creator& creator)
  {
    return put(_nextTrackId, creator) ? _nextTrackId++ : 0;
  }

  void MediaLibrary::remove(TrackId id)
  {
    _metaDb.del(_txn, id);
  }

  void MediaLibrary::update(TrackId id, const Creator& creator)
  {
    put(id, creator);
  }

  const Track* MediaLibrary::get(TrackId id)
  {
    lmdb::val key{&id, sizeof(TrackId)};
    lmdb::val value;
    _metaDb.get(_txn, key, value);
    return GetTrack(value.data());
  }

  bool MediaLibrary::put(TrackId id, const Creator& creator)
  {
    flatbuffers::Offset<Track> track = creator(_fbb);
    _fbb.Finish(track);

    lmdb::val key{&id, sizeof(TrackId)};
    lmdb::val value{_fbb.GetBufferPointer(), _fbb.GetSize()};
    bool success = _metaDb.put(_txn, key, value);
    _fbb.Clear();

    return success;
  }

  MediaLibrary::Iterator::Iterator()
    : _cursor{nullptr}, _key{nullptr, 0}
  {
  }

  MediaLibrary::Iterator::Iterator(lmdb::cursor&& cursor)
    : _cursor{std::move(cursor)}
  {
    increment();
  }

  MediaLibrary::Iterator::Iterator(const Iterator& other)
    : _cursor{nullptr},
      _key{other._key.data(), other._key.size()},
      _value{other._value.data(), other._value.size()}
  {
    if (other._cursor.handle() != nullptr)
    {
      _cursor = lmdb::cursor::open(other._cursor.txn(), other._cursor.dbi());

      if (_key.data() != nullptr)
      {
        TrackId* id = _key.data<TrackId>();
        _cursor.find(*id);
      }
    }
  }

  MediaLibrary::Iterator::Iterator(Iterator&& other) = default;

  bool MediaLibrary::Iterator::equal(const Iterator& other) const
  {
    return _key.data() == other._key.data();
  }

  void MediaLibrary::Iterator::increment()
  {
    if (!_cursor.get(_key, _value, MDB_NEXT))
    {
      _key = lmdb::val{nullptr, 0};
    }
    else
    {
      _track = GetTrack(_value.data());
    }
  }

  MediaLibrary::Value MediaLibrary::Iterator::dereference() const
  {
    return Value(*_key.data<TrackId>(), _track);
  }
}

