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

#include <boost/iterator/transform_iterator.hpp>
#include <rs/lmdb/Database.h>

namespace rs::core
{
  class ResourceStore
  {
  public:
    using Id = std::uint64_t;
    using Reader = lmdb::Database::Reader;
    class Writer;

    ResourceStore(lmdb::Environment& env, const std::string& db) : _database{env, db} {}

    Reader reader(lmdb::ReadTransaction& txn) const { return _database.reader(txn); };
    Writer writer(lmdb::WriteTransaction& txn);

  private:
    lmdb::Database _database;
  };

  class ResourceStore::Writer
  {
  public:
    Id create(boost::asio::const_buffer);
    bool del(Id id) { return _writer.del(id); }

  private:
    explicit Writer(lmdb::Database::Reader&& reader, lmdb::Database::Writer&& writer)
      : _reader{reader}
      , _writer{std::move(writer)}
    {
    }

    lmdb::Database::Reader _reader;
    lmdb::Database::Writer _writer;
    friend class ResourceStore;
  };

}
