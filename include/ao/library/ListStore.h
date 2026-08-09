// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListView.h>
#include <ao/lmdb/Database.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <utility>

namespace ao::library
{
  namespace detail
  {
    class LibraryIdentity;
    class PhysicalStoreAccess;
  }

  class ReadTransaction;
  class LibraryWrite;
  class WriteTransaction;
  class MusicLibrary;

  /**
   * ListStore - Binary storage for lists using ListLayout.
   */
  class ListStore final
  {
  public:
    class Reader;
    class Writer;

    Reader reader(ReadTransaction const& transaction) const;
    Reader reader(LibraryWrite const& write) const;
    Reader reader(WriteTransaction const& transaction) const;

  private:
    Writer writer(WriteTransaction& transaction) const;
    ListStore(lmdb::Database db, detail::LibraryIdentity const& identity);

    lmdb::Database _database;
    detail::LibraryIdentity const* _identity;

    friend class MusicLibrary;
    friend class ListWriter;
    friend class WriteTransaction;
    friend class detail::PhysicalStoreAccess;
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

    // Absence is the only normal miss. Native storage faults are fatal on a
    // read snapshot, and a post-open structural breach is an invariant fault.
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
    // Aborts through AO_INVARIANT when a row violates the open-time proof.
    value_type operator*() const;

  private:
    Iterator(lmdb::Database::Reader::Iterator&& iter);

    lmdb::Database::Reader::Iterator _iter;
    friend class Reader;
  };

  /**
   * ListStore::Writer - Write access to lists.
   */
  class [[nodiscard]] ListStore::Writer final
  {
  public:
    Result<ListId> create(ListBuilder::Prepared const& prepared);
    Result<> update(ListId id, ListBuilder::Prepared const& prepared);
    // Returns true if a row was removed, false if the id was absent.
    bool remove(ListId id);
    Result<> clear();

    // Absence is the only normal miss. Write-snapshot faults abort the root
    // transaction; a post-open structural breach is an invariant fault.
    std::optional<ListView> get(ListId id) const;

  private:
    explicit Writer(lmdb::Database::Writer&& writer);

    lmdb::Database::Writer _writer;
    friend class ListStore;
  };
} // namespace ao::library
