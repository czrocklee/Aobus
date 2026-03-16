// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/ListLayout.h>
#include <rs/lmdb/Database.h>
#include <rs/utility/TaggedInteger.h>

#include <boost/iterator/transform_iterator.hpp>
#include <optional>
#include <span>
#include <vector>

namespace rs::core
{

  /**
   * ListStore - Binary storage for lists using ListLayout.
   */
  class ListStore
  {
  public:
    struct IdTag
    {};

    using Id = utility::TaggedInteger<std::uint32_t, IdTag>;

    class Reader;
    class Writer;

    ListStore(lmdb::WriteTransaction& txn, std::string const& db);

    Reader reader(lmdb::ReadTransaction& txn) const;
    Writer writer(lmdb::WriteTransaction& txn);

  private:
    lmdb::Database _database;
  };

  /**
   * ListStore::Reader - Read-only access to lists.
   */
  class ListStore::Reader
  {
  public:
    class Iterator;

    [[nodiscard]] Iterator begin() const;
    [[nodiscard]] Iterator end() const;

    std::optional<ListView> get(Id id) const;

  private:
    Reader(lmdb::Database::Reader&& reader);

    lmdb::Database::Reader _reader;
    friend class ListStore;
  };

  /**
   * ListStore::Reader::Iterator - Iterator over lists.
   */
  class ListStore::Reader::Iterator
  {
  public:
    using value_type = std::pair<Id, ListView>;

    Iterator() = default;
    Iterator(Iterator const& other) = default;

    bool operator==(Iterator const& other) const;
    Iterator& operator++();
    value_type operator*() const;

  private:
    Iterator(lmdb::Database::Reader::Iterator&& iter);

    lmdb::Database::Reader::Iterator _iter;
    ListView _view;
    friend class Reader;
  };

  /**
   * ListStore::Writer - Write access to lists.
   */
  class ListStore::Writer
  {
  public:
    Writer() = default;

    std::pair<Id, ListView> create(std::span<std::byte const> data);
    ListView update(Id id, std::span<std::byte const> data);
    bool del(Id id);

    std::optional<ListView> get(Id id) const;

  private:
    explicit Writer(lmdb::Database::Writer&& writer);

    lmdb::Database::Writer _writer;
    friend class ListStore;
  };

} // namespace rs::core
