// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/library/ListView.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <utility>

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

    explicit ListStore(lmdb::Database db);

    Reader reader(lmdb::ReadTransaction const& txn) const;
    Writer writer(lmdb::WriteTransaction& txn);

  private:
    lmdb::Database _database;
  };

  /**
   * ListStore::Reader - Read-only access to lists.
   */
  class ListStore::Reader final
  {
  public:
    struct EndSentinel
    {};
    class Iterator;

    Iterator begin() const;
    EndSentinel end() const { return {}; }

    // Absence is the only recoverable miss; storage faults throw (see lmdb).
    std::optional<ListView> get(ListId id) const;

  private:
    Reader(lmdb::Database::Reader reader);

    lmdb::Database::Reader _reader;
    friend class ListStore;
  };

  /**
   * ListStore::Reader::Iterator - Iterator over lists.
   */
  class ListStore::Reader::Iterator
  {
  public:
    using value_type = std::pair<ListId, ListView>;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::input_iterator_tag;

    Iterator() = default;
    Iterator(Iterator const&) = delete;
    ~Iterator() = default;
    Iterator& operator=(Iterator const&) = delete;
    Iterator(Iterator&&) = default;
    Iterator& operator=(Iterator&&) = default;

    bool operator==(Iterator const& other) const;
    bool operator==(EndSentinel /*unused*/) const { return *this == Iterator{}; }
    Iterator& operator++();
    void operator++(std::int32_t) { ++*this; }
    value_type operator*() const;

  private:
    Iterator(lmdb::Database::Reader::Iterator&& iter);

    lmdb::Database::Reader::Iterator _iter;
    friend class Reader;
  };

  /**
   * ListStore::Writer - Write access to lists.
   */
  class ListStore::Writer final
  {
  public:
    Result<std::pair<ListId, ListView>> create(std::span<std::byte const> data);
    Result<> update(ListId id, std::span<std::byte const> data);
    // Returns true if a row was removed, false if the id was absent.
    bool remove(ListId id);
    Result<> clear();

    std::optional<ListView> get(ListId id) const;

  private:
    explicit Writer(lmdb::Database::Writer&& writer);

    lmdb::Database::Writer _writer;
    friend class ListStore;
  };
} // namespace ao::library
