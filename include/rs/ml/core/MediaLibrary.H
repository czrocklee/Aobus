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

#include <rs/ml/core/Protocol.capnp.h>
#include <lmdbxx/lmdb++.h>
#include <capnp/serialize.h>
#include <capnp/message.h>
#include <boost/iterator/iterator_facade.hpp>

#include <cassert>
#include <functional>
#include <iostream>

namespace rs::ml::core
{
  class MediaLibrary
  {
  public:
    using TrackId = std::uint64_t;
    class Iterator;

    MediaLibrary(const std::string& folder);

    ~MediaLibrary();

    Iterator begin() const;

    Iterator end() const;

    template<typename Encoder>
    TrackId add(Encoder&& encoder);

    void remove(TrackId id);

    template<typename Encoder>
    void update(TrackId id, Encoder&& encoder);

    template<typename Accessor>
    void get(TrackId id, Accessor&& accesor);

  private:
    struct Decoder;
    using Value = std::pair<TrackId, Decoder>;

    template<typename Encoder>
    bool put(TrackId id, Encoder&& encoder);

  private:
    lmdb::env _env;
    lmdb::txn _txn;
    lmdb::dbi _metaDb;
    lmdb::dbi _resourceDb;
    TrackId _nextTrackId;
  };

  struct MediaLibrary::Decoder
  {
    template<typename Accessor>
    void operator()(Accessor&& accessor) const
    {
      decode(value, std::forward<Accessor>(accessor));
    }
    
    template<typename Accessor>
    static void decode(const lmdb::val& buffer, Accessor&& accessor)
    {
      using ::capnp::word;
      assert(buffer.size() % sizeof(word) == 0);
      kj::ArrayPtr<const word> segment{buffer.data<word>(), buffer.size() / sizeof(word)};
      ::capnp::FlatArrayMessageReader message{segment};
      std::invoke(std::forward<Accessor>(accessor), message.getRoot<Track>());
    }

    const lmdb::val& value;
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

    friend class MediaLibrary;
  };

  template<typename Encoder>
  MediaLibrary::TrackId MediaLibrary::add(Encoder&& encoder)
  {
    return put(_nextTrackId, std::forward<Encoder>(encoder)) ? _nextTrackId++ : 0;
  }

  template<typename Encoder>
  void MediaLibrary::update(TrackId id, Encoder&& encoder)
  {
    put(id, std::forward<Encoder>(encoder));
  }

  template<typename Accessor>
  void MediaLibrary::get(TrackId id, Accessor&& accessor)
  {
    lmdb::val key{&id, sizeof(TrackId)};
    lmdb::val value;
    _metaDb.get(_txn, key, value);
    Decoder::decode(value, std::forward<Accessor>(accessor));
  }

  template<typename Encoder>
  bool MediaLibrary::put(TrackId id, Encoder&& encoder)
  {
    ::kj::FixedArray<::capnp::word, 1024> tmpBuffer;
    ::capnp::MallocMessageBuilder message(tmpBuffer);

    std::invoke(encoder, message.initRoot<Track>());

    lmdb::val key{&id, sizeof(TrackId)};
    lmdb::val value{nullptr, ::capnp::computeSerializedSizeInWords(message) * sizeof(::capnp::word)};

    if (!_metaDb.put(_txn, key, value, MDB_RESERVE))
    {
      return false;
    }

    kj::ArrayPtr<::capnp::byte> buffer{value.data<::capnp::byte>(), value.size() / sizeof(::capnp::byte)};
    kj::ArrayOutputStream stream{buffer};
    ::capnp::writeMessage(stream, message);
    return true;
  }

}
