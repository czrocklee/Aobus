// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/iterator/transform_iterator.hpp>
#include <span>
#include <rs/lmdb/Database.h>

namespace rs::core
{
  class ResourceStore
  {
  public:
    using Id = std::uint32_t;
    using Reader = lmdb::Database::Reader;
    class Writer;

    ResourceStore(lmdb::WriteTransaction& txn, std::string const& db) : _database{txn, db} {}

    Reader reader(lmdb::ReadTransaction& txn) const { return _database.reader(txn); };
    Writer writer(lmdb::WriteTransaction& txn);

  private:
    lmdb::Database _database;
  };

  class ResourceStore::Writer
  {
  public:
    Id create(std::span<std::byte const> data);
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
