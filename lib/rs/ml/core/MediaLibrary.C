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
#include <rs/ml/core/UpdateObserverable.H>
#include <flatbuffers/flatbuffers.h>
#include "ReaderImplCache.H"

namespace
{
  struct BuilderGuard
  {
    flatbuffers::FlatBufferBuilder& fbb;
    ~BuilderGuard() { fbb.Clear(); }
  };
}

namespace rs::ml::core
{
  using Writer = MediaLibrary::Writer;
  using Reader = MediaLibrary::Reader;

  struct MediaLibrary::Impl
  {
    lmdb::env env = lmdb::env::create();
    lmdb::dbi trackDb = {MDB_dbi{}};
    lmdb::dbi resourceDb = {MDB_dbi{}};
    TrackId nextTrackId;
    std::mutex writerMutex;
    UpdateObserverable<TrackId, const Track*> observerable;
    flatbuffers::FlatBufferBuilder fbb;
    impl::ReaderImplCache readerImplCache;
  };
  
  MediaLibrary::MediaLibrary(const std::string& folder)
    : _impl{std::make_unique<Impl>()}
  {
    _impl->env.set_mapsize(1UL * 1024UL * 1024UL * 1024UL);
    _impl->env.set_max_dbs(2);
    _impl->env.open(folder.c_str(), MDB_NOTLS, 0664);

    auto txn = lmdb::txn::begin(_impl->env);
    _impl->trackDb = lmdb::dbi::open(txn, "track", MDB_CREATE | MDB_INTEGERKEY);
    _impl->resourceDb = lmdb::dbi::open(txn, "resource", MDB_CREATE);
    
    {  
      lmdb::val key;
      auto cursor = lmdb::cursor::open(txn, _impl->trackDb);
      _impl->nextTrackId = cursor.get(key, MDB_LAST) ? *key.data<TrackId>() + 1 : 1;
    }

    txn.commit();
  }

  MediaLibrary::~MediaLibrary()
  {
  }

  Reader MediaLibrary::reader() const
  {
    return {*_impl};
  }

  Writer MediaLibrary::writer()
  {
    return {*_impl};
  }

  void MediaLibrary::attach(Observer& observer)
  {
    std::lock_guard{_impl->writerMutex};
    _impl->observerable.attach(observer);
  }

  void MediaLibrary::detach(Observer& observer)
  {
    std::lock_guard{_impl->writerMutex};
    _impl->observerable.detach(observer);
  }


  Reader::Reader(MediaLibrary::Impl& mli)
    : _mli{mli}, 
      _impl{mli.readerImplCache.get(mli.env, mli.trackDb)}
  {
  }

  Reader::~Reader()
  {
    _mli.readerImplCache.put(std::move(_impl));
  }

  Reader::Iterator Reader::begin() const
  {
    return {lmdb::cursor::open(_impl.first, _mli.trackDb)};
  }

  Reader::Iterator Reader::end() const
  {
    return {};
  }

  const Track* Reader::operator[](TrackId id) const
  {
    lmdb::val key{&id, sizeof(TrackId)};
    lmdb::val value;
    return _impl.second.get(key, value, MDB_SET_KEY) ? GetTrack(value.data()) : nullptr;
  }

  Reader::Iterator::Iterator()
    : _cursor{nullptr}, _value{}
  {
  }

  Reader::Iterator::Iterator(lmdb::cursor&& cursor)
    : _cursor{std::move(cursor)}
  {
    increment();
  }

  Reader::Iterator::Iterator(const Iterator& other)
    : _cursor{nullptr}, _value{other._value}
  {
    if (other._cursor != nullptr)
    {
      _cursor = lmdb::cursor::open(other._cursor.txn(), other._cursor.dbi());
      _cursor.find(_value.first);
    }
  }

  Reader::Iterator::Iterator(Iterator&& other) = default;

  bool Reader::Iterator::equal(const Iterator& other) const
  {
    return _cursor == other._cursor && _value.first == other._value.first;
  }

  void Reader::Iterator::increment()
  {
    lmdb::val key, value;

    if (!_cursor.get(key, value, MDB_NEXT))
    {
      _value = Value{};
      _cursor.close();
    }
    else
    {
      _value.first = *key.data<TrackId>();
      _value.second = GetTrack(value.data());
    }
  }

  const Reader::Value& Reader::Iterator::dereference() const
  {
    return _value;
  }

  Writer::Writer(MediaLibrary::Impl& mli)
    : _mli{mli}, _txn{lmdb::txn::begin(_mli.env)}, _lock{_mli.writerMutex}
    
  {
    _mli.observerable.beginUpdate();
  }

  Writer::~Writer()
  {
    _txn.commit();
    _mli.observerable.endUpdate();
  }


  TrackId Writer::create(const Creator& creator)
  {
    return create(creator(_mli.fbb));
  }

  TrackId Writer::create(const TrackT& track)
  {
    return create(Track::Pack(_mli.fbb, &track));
  }

  TrackId Writer::create(flatbuffers::Offset<Track> track)
  {
    BuilderGuard guard{_mli.fbb};
    _mli.fbb.Finish(track);

    lmdb::val key{&_mli.nextTrackId, sizeof(TrackId)};
    lmdb::val value{_mli.fbb.GetBufferPointer(), _mli.fbb.GetSize()};
    
    if (!_mli.trackDb.put(_txn, key, value, MDB_NOOVERWRITE | MDB_APPEND))
    {
      return 0;
    }
    
    _mli.observerable.create(_mli.nextTrackId, GetTrack(value.data()));
    return _mli.nextTrackId++;
  }

  bool Writer::modify(TrackId id, const TrackT& track)
  {
    BuilderGuard guard{_mli.fbb};
    _mli.fbb.Finish(Track::Pack(_mli.fbb, &track));

    lmdb::val key{&id, sizeof(TrackId)};
    lmdb::val value{_mli.fbb.GetBufferPointer(), _mli.fbb.GetSize()};
    
    if (!_mli.trackDb.put(_txn, key, value))
    {
      return false;
    }
    
    _mli.observerable.modify(id, GetTrack(value.data()));
    return true;
  }

  void Writer::remove(TrackId id)
  {
    lmdb::val key{&id, sizeof(TrackId)}, value{};
    
    if (_mli.trackDb.get(_txn, key, value))
    {
      _mli.observerable.remove(id, GetTrack(value.data()));
      _mli.trackDb.del(_txn, id);
    }
  }

}

