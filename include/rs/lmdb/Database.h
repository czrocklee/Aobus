// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/iterator/iterator_facade.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <rs/lmdb/Transaction.h>
#include <span>
#include <vector>

namespace rs::lmdb
{
  class Database
  {
  public:
    class Reader;
    class Writer;

    Database() = default;
    Database(WriteTransaction& txn, std::string const& db);
    Database(ReadTransaction& txn, std::string const& db);
    ~Database();

    Reader reader(ReadTransaction& txn) const;
    Writer writer(WriteTransaction& txn);

  private:
    MDB_dbi _dbi = (std::numeric_limits<MDB_dbi>::max)();
  };

  class Database::Reader
  {
  public:
    using Value = std::pair<std::uint32_t, std::span<std::byte const>>;
    class Iterator;

    Iterator begin() const;
    Iterator end() const;
    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;

  protected:
    Reader(MDB_dbi dbi, MDB_txn* txn);

    MDB_dbi _dbi;
    MDB_txn* _txn;
    friend class Database;
  };

  class Database::Reader::Iterator : public boost::iterator_facade<Iterator, Value const, boost::forward_traversal_tag>
  {
  public:
    friend class boost::iterator_core_access;

    Iterator();
    Iterator(Iterator const& other);
    Iterator(Iterator&& other) noexcept;
    ~Iterator();
    bool equal(Iterator const& other) const;
    void increment();
    Value const& dereference() const;

  private:
    struct CursorDeleter
    {
      void operator()(MDB_cursor* cur) const { mdb_cursor_close(cur); }
    };
    Iterator(MDB_cursor* cursor);

    std::unique_ptr<MDB_cursor, CursorDeleter> _cursor;
    Value _value;
    friend class Reader;
  };

  class Database::Writer
  {
  public:
    Writer(Writer&&) noexcept;
    ~Writer();

    void create(std::uint32_t id, std::span<std::byte const> data);
    std::span<std::byte> create(std::uint32_t id, std::size_t size);
    std::uint32_t append(std::span<std::byte const> data);
    std::pair<std::uint32_t, std::span<std::byte>> append(std::size_t size);
    void update(std::uint32_t id, std::span<std::byte const> data);
    bool del(std::uint32_t id);
    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;

  private:
    Writer(MDB_dbi dbi, WriteTransaction& txn);

    MDB_dbi _dbi;
    WriteTransaction& _txn;
    std::unique_ptr<MDB_cursor, decltype([](auto* cur) { mdb_cursor_close(cur); })> _cursor;
    std::uint32_t _lastId = std::numeric_limits<std::uint32_t>::max();
    friend class Database;
  };

}
