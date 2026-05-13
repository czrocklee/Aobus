// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListView.h>
#include <ao/lmdb/Database.h>

#include <optional>
#include <span>
#include <vector>

namespace ao::library
{
  /**
   * ListStore - Binary storage for lists using ListLayout.
   */
  class ListStore final
  {
  public:
    class Reader;
    class Writer;

    explicit ListStore(ao::lmdb::Database db);

    Reader reader(ao::lmdb::ReadTransaction const& txn) const;
    Writer writer(ao::lmdb::WriteTransaction& txn);

  private:
    ao::lmdb::Database _database;
  };

  /**
   * ListStore::Reader - Read-only access to lists.
   */
  class ListStore::Reader final
  {
  public:
    class Iterator;

    Iterator begin() const;
    Iterator end() const;

    std::optional<ListView> get(ListId id) const;

  private:
    Reader(ao::lmdb::Database::Reader reader);

    ao::lmdb::Database::Reader _reader;
    friend class ListStore;
  };

  /**
   * ListStore::Reader::Iterator - Iterator over lists.
   */
  class ListStore::Reader::Iterator
  {
  public:
    using value_type = std::pair<ListId, ListView>;

    Iterator() = default;
    Iterator(Iterator const&) = default;
    // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
    ~Iterator() = default;
    Iterator& operator=(Iterator const&) = default;
    Iterator(Iterator&&) = default;
    Iterator& operator=(Iterator&&) = default;

    bool operator==(Iterator const& other) const;
    Iterator& operator++();
    value_type operator*() const;

  private:
    Iterator(ao::lmdb::Database::Reader::Iterator&& iter);

    ao::lmdb::Database::Reader::Iterator _iter;
    friend class Reader;
  };

  /**
   * ListStore::Writer - Write access to lists.
   */
  class ListStore::Writer final
  {
  public:
    std::pair<ListId, ListView> create(std::span<std::byte const> data);
    void update(ListId id, std::span<std::byte const> data);
    bool del(ListId id);
    void clear();

    std::optional<ListView> get(ListId id) const;

  private:
    explicit Writer(ao::lmdb::Database::Writer&& writer);

    ao::lmdb::Database::Writer _writer;
    friend class ListStore;
  };
} // namespace ao::library
