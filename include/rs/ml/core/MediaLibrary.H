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
#include <lmdbxx/lmdb++.h>
#include <boost/iterator/iterator_facade.hpp>
#include <mutex>

namespace rs::ml::core
{
  class MediaLibrary
  {
  public:
    class Reader;
    class Writer;

    MediaLibrary(const std::string& folder);
    ~MediaLibrary();

    Reader reader() const;
    Writer writer();

    using Observer = UpdateObserver<TrackId, const Track*>;

    void attach(Observer& observer);
    void detach(Observer& observer);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };


  class MediaLibrary::Reader
  {
  public:
    using Value = std::pair<TrackId, const Track*>;
    class Iterator;

    Reader(MediaLibrary::Impl& impl);
    ~Reader();
    Iterator begin() const;
    Iterator end() const;
    const Track* operator[](TrackId id) const;

  private:
    MediaLibrary::Impl& _mli;
    using Impl = std::pair<lmdb::txn, lmdb::cursor>;
    mutable Impl _impl;
  };

  class MediaLibrary::Reader::Iterator : 
    public boost::iterator_facade<Iterator, const Value, boost::forward_traversal_tag>
  {
  public:
    friend class boost::iterator_core_access;

    Iterator();
    Iterator(lmdb::cursor&& cursor);
    Iterator(const Iterator& other);
    Iterator(Iterator&& other);
    bool equal(const Iterator& other) const;
    void increment();
    const Value& dereference() const;

  private:
    lmdb::cursor _cursor;
    Value _value;
  };

  class MediaLibrary::Writer
  {
  public:
    Writer(MediaLibrary::Impl& impl);
    ~Writer();

    TrackId create(const TrackT& track);
    bool modify(TrackId id, const TrackT& track);
    void remove(TrackId id);
  
  private:
    MediaLibrary::Impl& _mli;
    lmdb::txn _txn;
    std::lock_guard<std::mutex> _lock;
  };

}
