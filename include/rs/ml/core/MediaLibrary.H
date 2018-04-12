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

#include <lmdbxx/lmdb++.h>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/optional.hpp>
#include <capnp/serialize.h>
#include <flatbuffers/flatbuffers.h>

#include <memory.h>
#include <functional>



auto reader = ml.reader();
reader.release();
reader.aquire();

reader.release();


struct Listener
{
  void begin()

  void onUpdate(TrackId id, const Track*);

  void onDelete(Track id);

  void onCreate(Track id, const Track*);

  void end();
};


namespace rs::ml::core
{
  class Track;  

  class MediaLibrary
  {
  public:
    using TrackId = std::uint64_t;
    using Creator = std::function<flatbuffers::Offset<Track>(flatbuffers::FlatBufferBuilder&)>;
    using Value = std::pair<TrackId, const Track*>;
    class Iterator;

    MediaLibrary(const std::string& folder);

    ~MediaLibrary();

    Iterator begin() const;

    Iterator end() const;

    TrackId add(const Creator& creator);

    void remove(TrackId id);

    void update(TrackId id, const Creator& creator);

    const Track* get(TrackId id);

  private:
    bool put(TrackId id, const Creator& creator);

  private:
    lmdb::env _env;
    lmdb::txn _txn;
    lmdb::dbi _metaDb;
    lmdb::dbi _resourceDb;
    TrackId _nextTrackId;
    flatbuffers::FlatBufferBuilder _fbb;
  };

  class MediaLibrary::Iterator : public boost::iterator_facade<Iterator, Value, boost::forward_traversal_tag, Value>
  {
  public:
    friend class boost::iterator_core_access;

    Iterator();

    Iterator(lmdb::cursor&& cursor);

    Iterator(const Iterator& other);

    Iterator(Iterator&& other);

    bool equal(const Iterator& other) const;

    void increment();

    Value dereference() const;

  private:
    lmdb::cursor _cursor;
    lmdb::val _key;
    lmdb::val _value;
    const Track* _track;

    friend class MediaLibrary;
  };

}
