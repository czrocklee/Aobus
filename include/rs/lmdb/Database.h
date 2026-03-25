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

    Database(WriteTransaction& txn, std::string const& db);
    Database(ReadTransaction& txn, std::string const& db);

    // movable
    Database(Database&&) = default;
    Database& operator=(Database&&) = default;

    // not copyable
    Database(Database const&) = delete;
    Database& operator=(Database const&) = delete;

    ~Database() = default;

    Reader reader(ReadTransaction& txn) const;
    Writer writer(WriteTransaction& txn) const;

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

    ~Reader() = default;

    // movable
    Reader(Reader&&) = default;
    Reader& operator=(Reader&&) = default;

    // not copyable
    Reader(Reader const&) = delete;
    Reader& operator=(Reader const&) = delete;

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
    Iterator& operator=(Iterator const&) = default;
    Iterator& operator=(Iterator&&) = default;
    bool equal(Iterator const& other) const;
    void increment();
    Value const& dereference() const;

  private:
    struct MdbCursorDeleter
    {
      void operator()(MDB_cursor* cur) const noexcept { mdb_cursor_close(cur); }
    };

    explicit Iterator(std::unique_ptr<MDB_cursor, MdbCursorDeleter> cursor);
    static auto create(MDB_txn* txn, MDB_dbi dbi);

    std::unique_ptr<MDB_cursor, MdbCursorDeleter> _cursor;
    Value _value;
    friend class Reader;
    friend class Writer;
  };

  class Database::Writer
  {
  public:
    ~Writer();

    // movable
    Writer(Writer&& other) noexcept = default;
    Writer& operator=(Writer&&) noexcept = default;

    // not copyable
    Writer(Writer const&) = delete;
    Writer& operator=(Writer const&) = delete;

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
    WriteTransaction* _txn;
    std::unique_ptr<MDB_cursor, Reader::Iterator::MdbCursorDeleter> _cursor;
    std::uint32_t _lastId = std::numeric_limits<std::uint32_t>::max();
    friend class Database;
  };

}
