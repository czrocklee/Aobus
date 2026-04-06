// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/iterator/transform_iterator.hpp>
#include <rs/core/Type.h>
#include <rs/lmdb/Database.h>
#include <span>

namespace rs::core
{

  class ResourceStore
  {
  public:
    using Reader = lmdb::Database::Reader;
    class Writer;

    explicit ResourceStore(lmdb::Database db) : _database{std::move(db)} {}

    Reader reader(lmdb::ReadTransaction& txn) const { return _database.reader(txn); };
    Writer writer(lmdb::WriteTransaction& txn);

  private:
    lmdb::Database _database;
  };

  class ResourceStore::Writer
  {
  public:
    ResourceId create(std::span<std::byte const> data);
    bool del(ResourceId id) { return _writer.del(id.value()); }

  private:
    explicit Writer(lmdb::Database::Reader&& reader, lmdb::Database::Writer&& writer)
      : _reader{std::move(reader)}
      , _writer{std::move(writer)}
    {
    }

    lmdb::Database::Reader _reader;
    lmdb::Database::Writer _writer;
    friend class ResourceStore;
  };

}
