// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/lmdb/Transaction.h>

#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace ao::lmdb
{
  class ReadTransaction;
  class WriteTransaction;

  /**
   * Database - Wrapper for an LMDB named database (DBI).
   */
  class Database final
  {
  public:
    enum class KeyKind : std::uint8_t
    {
      Integer,
      Blob
    };

    class Reader;
    class Writer;

    explicit Database(WriteTransaction& txn, std::string const& name, KeyKind kind = KeyKind::Integer);
    explicit Database(ReadTransaction& txn, std::string const& name, KeyKind kind = KeyKind::Integer);

    Reader reader(ReadTransaction const& txn) const;
    Writer writer(WriteTransaction& txn) const;

    KeyKind kind() const noexcept { return _kind; }

  private:
    ::MDB_dbi _dbi = std::numeric_limits<::MDB_dbi>::max();
    KeyKind _kind = KeyKind::Integer;
  };

  /**
   * Database::Reader - Read-only access to a database within a transaction.
   */
  class Database::Reader final
  {
  public:
    /**
     * KeyView - Strong view of a key, convertible to uint32_t for integer keys.
     */
    struct KeyView final : std::span<std::byte const>
    {
      using std::span<std::byte const>::span;
      operator std::uint32_t() const noexcept;
    };

    struct EndSentinel
    {};
    using Value = std::pair<KeyView, std::span<std::byte const>>;
    class Iterator;

    Iterator begin() const;
    EndSentinel end() const { return {}; }

    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;
    std::optional<std::span<std::byte const>> get(std::span<std::byte const> key) const;

    std::uint32_t maxKey() const;

    ~Reader() = default;

    // copyable and movable
    Reader(Reader const&) = default;
    Reader& operator=(Reader const&) = default;
    Reader(Reader&&) = default;
    Reader& operator=(Reader&&) = default;

    KeyKind kind() const noexcept { return _kind; }

  private:
    Reader(::MDB_dbi dbi, ::MDB_txn* txn, KeyKind kind);

    struct MdbCursorDeleter final
    {
      void operator()(::MDB_cursor* cur) const noexcept { ::mdb_cursor_close(cur); }
    };

    using CursorPtr = std::unique_ptr<::MDB_cursor, MdbCursorDeleter>;
    static CursorPtr create(::MDB_txn* txn, ::MDB_dbi dbi);

    ::MDB_dbi _dbi;
    ::MDB_txn* _txn;
    KeyKind _kind;

    friend class Database;
    friend class Writer;
  };

  /**
   * Database::Reader::Iterator - Bidirectional iterator over database entries.
   */
  class Database::Reader::Iterator final
  {
  public:
    using iterator_category = std::input_iterator_tag;
    using value_type = Reader::Value;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type const*;
    using reference = value_type const&;

    Iterator() = default;
    ~Iterator() = default;
    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) noexcept = default;

    // Not copyable because of the cursor
    Iterator(Iterator const&) = delete;
    Iterator& operator=(Iterator const&) = delete;

    reference operator*() const;
    pointer operator->() const;

    Iterator& operator++();
    void operator++(std::int32_t) { ++*this; }
    bool operator==(Iterator const& other) const;
    bool operator==(EndSentinel /*unused*/) const { return *this == Iterator{}; }

  private:
    Iterator(::MDB_txn* txn, ::MDB_dbi dbi, bool end);

    void next();

    Reader::CursorPtr _cursor;
    Reader::Value _value;

    friend class Reader;
  };

  /**
   * Database::Writer - Write access to a database within a transaction.
   */
  class Database::Writer final
  {
  public:
    ~Writer() noexcept;

    // Not copyable
    Writer(Writer const&) = delete;
    Writer& operator=(Writer const&) = delete;

    // Movable
    Writer(Writer&&) noexcept = default;
    Writer& operator=(Writer&&) noexcept = default;

    void create(std::uint32_t id, std::span<std::byte const> data);
    void create(std::span<std::byte const> key, std::span<std::byte const> data);

    std::span<std::byte> create(std::uint32_t id, std::size_t size);
    std::span<std::byte> create(std::span<std::byte const> key, std::size_t size);

    std::uint32_t maxKey() const noexcept { return _lastId; }
    std::uint32_t append(std::span<std::byte const> data);
    std::pair<std::uint32_t, std::span<std::byte>> append(std::size_t size);

    void update(std::uint32_t id, std::span<std::byte const> data);
    void update(std::span<std::byte const> key, std::span<std::byte const> data);

    std::span<std::byte> update(std::uint32_t id, std::size_t size);
    std::span<std::byte> update(std::span<std::byte const> key, std::size_t size);

    bool del(std::uint32_t id);
    bool del(std::span<std::byte const> key);

    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;
    std::optional<std::span<std::byte const>> get(std::span<std::byte const> key) const;

    void clear();

    KeyKind kind() const noexcept { return _kind; }

  private:
    Writer(::MDB_dbi dbi, WriteTransaction& txn, KeyKind kind);

    ::MDB_dbi _dbi;
    WriteTransaction* _txn;
    Reader::CursorPtr _cursor;
    std::uint32_t _lastId = 0; // Start from 1 (0 = null, so first append returns 1)
    KeyKind _kind;

    friend class Database;
  };
} // namespace ao::lmdb
