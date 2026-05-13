// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/lmdb/Database.h>
#include <span>

namespace ao::library
{
  class ResourceStore
  {
  public:
    using Reader = ao::lmdb::Database::Reader;
    class Writer;

    explicit ResourceStore(ao::lmdb::Database db)
      : _database{std::move(db)}
    {
    }

    Reader reader(ao::lmdb::ReadTransaction const& txn) const { return _database.reader(txn); };
    Writer writer(ao::lmdb::WriteTransaction& txn);

  private:
    ao::lmdb::Database _database;
  };

  class ResourceStore::Writer
  {
  public:
    ResourceId create(std::span<std::byte const> data);
    bool del(ResourceId id) { return _writer.del(id.value()); }
    void clear() { _writer.clear(); }

  private:
    explicit Writer(ao::lmdb::Database::Reader&& reader, ao::lmdb::Database::Writer&& writer)
      : _reader{std::move(reader)}, _writer{std::move(writer)}
    {
    }

    ao::lmdb::Database::Reader _reader;
    ao::lmdb::Database::Writer _writer;
    friend class ResourceStore;
  };
}
