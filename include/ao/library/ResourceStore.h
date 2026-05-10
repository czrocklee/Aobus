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
    using Reader = lmdb::Database::Reader;
    class Writer;

    explicit ResourceStore(lmdb::Database db)
      : _database{std::move(db)}
    {
    }

    Reader reader(lmdb::ReadTransaction const& txn) const { return _database.reader(txn); };
    Writer writer(lmdb::WriteTransaction& txn);

  private:
    lmdb::Database _database;
  };

  class ResourceStore::Writer
  {
  public:
    ResourceId create(std::span<std::byte const> data);
    bool del(ResourceId id) { return _writer.del(id.value()); }
    void clear() { _writer.clear(); }

  private:
    explicit Writer(lmdb::Database::Reader&& reader, lmdb::Database::Writer&& writer)
      : _reader{std::move(reader)}, _writer{std::move(writer)}
    {
    }

    lmdb::Database::Reader _reader;
    lmdb::Database::Writer _writer;
    friend class ResourceStore;
  };
}
