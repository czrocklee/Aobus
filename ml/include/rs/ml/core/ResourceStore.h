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

#include <rs/ml/core/LMDBDatabase.h>
#include <boost/iterator/transform_iterator.hpp>

namespace rs::ml::core
{
  class ResourceStore
  {
  public:
    using Id = std::uint64_t;
    using Reader = LMDBDatabase::Reader;
    class Writer;

    ResourceStore(lmdb::env& env, const std::string& db) : _database{env, db} {}

    Reader reader(LMDBReadTransaction& txn) const { return _database.reader(txn); };
    Writer writer(LMDBWriteTransaction& txn);

  private:
    LMDBDatabase _database;
  };

  class ResourceStore::Writer
  {
  public:
    Id create(boost::asio::const_buffer);
    bool del(Id id) { return _writer.del(id); }

  private:
    explicit Writer(LMDBDatabase::Reader&& reader, LMDBDatabase::Writer&& writer) 
      : _reader{std::move(reader)}, _writer{std::move(writer)} 
    {}

    LMDBDatabase::Reader _reader;
    LMDBDatabase::Writer _writer;
    friend class ResourceStore;
  };

}
