/*
 * MediaLibrary.cpp
 *
 *  Created on: Feb 3, 2017
 *      Author: rocklee
 */

#include <rml/MediaLibrary.H>
//#include <rml/Protocol_generated.h>
namespace
{
}

namespace rml
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

  void MediaLibrary::remove(TrackId id)
  {
    _metaDb.del(_txn, id);
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
  }

  MediaLibrary::Value MediaLibrary::Iterator::dereference() const
  {
    return {*_key.data<TrackId>(), Decoder{_value}};
  }
}

