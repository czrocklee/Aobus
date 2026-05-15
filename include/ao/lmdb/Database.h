// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/lmdb/Transaction.h>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ao::lmdb
{
  class Database final
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

    Reader reader(ReadTransaction const& txn) const;
    Writer writer(WriteTransaction& txn) const;

  private:
    ::MDB_dbi _dbi = std::numeric_limits<::MDB_dbi>::max();
  };

  class Database::Reader final
  {
  public:
    using Value = std::pair<std::uint32_t, std::span<std::byte const>>;
    class Iterator;

    Iterator begin() const;
    Iterator end() const;
    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;
    std::uint32_t maxKey() const;

    ~Reader() = default;

    // movable
    Reader(Reader&&) = default;
    Reader& operator=(Reader&&) = default;

    // not copyable
    Reader(Reader const&) = delete;
    Reader& operator=(Reader const&) = delete;

  private:
    Reader(::MDB_dbi dbi, ::MDB_txn* txn);

    struct MdbCursorDeleter final
    {
      void operator()(::MDB_cursor* cur) const noexcept { ::mdb_cursor_close(cur); }
    };

    using CursorPtr = std::unique_ptr<::MDB_cursor, MdbCursorDeleter>;
    static CursorPtr create(::MDB_txn* txn, ::MDB_dbi dbi);

    ::MDB_dbi _dbi;
    ::MDB_txn* _txn;

    friend class Database;
    friend class Writer;
  };

  class Database::Reader::Iterator final
  {
  public:
    // Standard iterator traits
    using difference_type = std::ptrdiff_t;     // NOLINT
    using value_type = Value const;             // NOLINT
    using pointer = Value const*;               // NOLINT
    using reference = Value const&;             // NOLINT
    using iterator_category = std::forward_iterator_tag; // NOLINT

    Iterator();
    Iterator(Iterator const& other);
    Iterator(Iterator&& other) noexcept;
    ~Iterator() noexcept;
    Iterator& operator=(Iterator const& /*other*/);
    Iterator& operator=(Iterator&&) = default;

    // Forward iterator operations
    reference operator*() const { return dereference(); }

    pointer operator->() const { return &dereference(); }

    Iterator& operator++()
    {
      increment();
      return *this;
    }

    Iterator operator++(int)
    {
      auto tmp = *this;
      increment();
      return tmp;
    }

    bool operator==(Iterator const& other) const
    {
      return (_cursor != nullptr) == (other._cursor != nullptr) && _value.first == other._value.first;
    }

    bool operator!=(Iterator const& other) const { return !(*this == other); }

  private:
    void increment();
    Value const& dereference() const;

    explicit Iterator(CursorPtr cursor);

    CursorPtr _cursor;
    Value _value;

    friend class Reader;
  };

  class Database::Writer final
  {
  public:
    ~Writer() noexcept;

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
    std::span<std::byte> update(std::uint32_t id, std::size_t size);
    bool del(std::uint32_t id);
    void clear();
    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;

  private:
    Writer(::MDB_dbi dbi, WriteTransaction& txn);

    ::MDB_dbi _dbi;
    WriteTransaction* _txn;
    Reader::CursorPtr _cursor;
    std::uint32_t _lastId = 0; // Start from 1 (0 = null, so first append returns 1)

    friend class Database;
  };
}
