// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// LMDB native handles, kept opaque (see Environment.h).
struct MDB_cursor;
struct MDB_txn;

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

    static Result<Database> open(WriteTransaction& txn, std::string const& name, KeyKind kind = KeyKind::Integer);
    static Result<Database> openExisting(WriteTransaction& txn, std::string const& name);
    static Result<Database> openExisting(WriteTransaction& txn, std::string const& name, KeyKind kind);
    static Database main(WriteTransaction& txn);

    Reader reader(ReadTransaction const& txn) const;
    Writer writer(WriteTransaction& txn) const;

    KeyKind kind() const noexcept { return _kind; }
    Result<> validateExactKeyKind(std::string_view databaseName, KeyKind expected) const;

  private:
    Database(DbiHandle dbi, KeyKind kind, std::uint32_t nativeFlags);
    static bool transactionOwned(ReadTransaction const& transaction) noexcept;

    DbiHandle _dbi = std::numeric_limits<DbiHandle>::max();
    KeyKind _kind = KeyKind::Integer;
    std::uint32_t _nativeFlags = 0;
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
      // Coerce an admitted integer key to uint32. A size mismatch violates the
      // database/key-kind invariant and terminates instead of yielding a bogus id.
      operator std::uint32_t() const;
    };

    struct EndSentinel
    {};
    using Value = std::pair<KeyView, std::span<std::byte const>>;
    class Iterator;

    Iterator begin() const;
    EndSentinel end() const { return {}; }

    // Point lookups. A missing key is normal. Other native faults are fatal for
    // an exposed read snapshot and use the private transaction carrier when the
    // reader belongs to a write/open transaction.
    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;
    std::optional<std::span<std::byte const>> get(std::span<std::byte const> key) const;

    // Number of rows visible to this transaction. Fault ownership follows the
    // same read-snapshot versus transaction phase split.
    std::size_t entryCount() const;

    // Largest integer key, or 0 when the database is empty.
    std::uint32_t maxKey() const;

    ~Reader() = default;

    // copyable and movable
    Reader(Reader const&) = default;
    Reader& operator=(Reader const&) = default;
    Reader(Reader&&) = default;
    Reader& operator=(Reader&&) = default;

    KeyKind kind() const noexcept { return _kind; }

  private:
    Reader(DbiHandle dbi, MDB_txn* txn, ReadTransaction const& owner, KeyKind kind);

    struct MdbCursorDeleter final
    {
      void operator()(MDB_cursor* cur) const noexcept;
    };

    using CursorPtr = std::unique_ptr<MDB_cursor, MdbCursorDeleter>;
    static CursorPtr create(MDB_txn* txn, ReadTransaction const& owner, DbiHandle dbi);
    void ensureActive() const;

    DbiHandle _dbi;
    MDB_txn* _txn;
    ReadTransaction const* _owner;
    KeyKind _kind;

    friend class Database;
    friend class Writer;
  };

  /**
   * Database::Reader::Iterator - Input iterator over database entries.
   *
   * Cursor EOF (MDB_NOTFOUND) is normal and compares equal to EndSentinel.
   * Other cursor failures use the owner selected by the transaction: exposed
   * read snapshots terminate, while write/open transactions unwind through the
   * private transaction carrier so their root owner can abort and return a
   * typed failure.
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
    ~Iterator() noexcept;
    Iterator(Iterator&& other) noexcept;
    Iterator& operator=(Iterator&& other) noexcept;

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
    Iterator(MDB_txn* txn, ReadTransaction const& owner, DbiHandle dbi, bool end);

    void ensureActive() const;
    void releaseFinishedCursor() noexcept;
    void next();

    Reader::CursorPtr _cursorPtr;
    Reader::Value _value;
    ReadTransaction const* _owner = nullptr;

    friend class Reader;
  };

  /**
   * Database::Writer - Write access to a database within a transaction.
   */
  class [[nodiscard]] Database::Writer final
  {
  public:
    ~Writer() noexcept;

    // Not copyable
    Writer(Writer const&) = delete;
    Writer& operator=(Writer const&) = delete;

    // Movable
    Writer(Writer&& other) noexcept;
    Writer& operator=(Writer&& other) noexcept;

    Result<> create(std::uint32_t id, std::span<std::byte const> data);
    Result<> create(std::span<std::byte const> key, std::span<std::byte const> data);

    // MDB_RESERVE writes. The returned span must be filled completely before
    // any subsequent LMDB update in the transaction and must not be read or
    // written after that update or after the transaction finishes.
    Result<std::span<std::byte>> create(std::uint32_t id, std::size_t size);
    Result<std::span<std::byte>> create(std::span<std::byte const> key, std::size_t size);

    std::uint32_t maxKey() const noexcept { return _lastId; }
    Result<std::uint32_t> append(std::span<std::byte const> data);
    Result<std::pair<std::uint32_t, std::span<std::byte>>> append(std::size_t size);

    Result<> update(std::uint32_t id, std::span<std::byte const> data);
    Result<> update(std::span<std::byte const> key, std::span<std::byte const> data);

    // The same MDB_RESERVE lifetime and complete-fill contract applies here.
    Result<std::span<std::byte>> update(std::uint32_t id, std::size_t size);
    Result<std::span<std::byte>> update(std::span<std::byte const> key, std::size_t size);

    // Delete a key. Returns true if a row was removed, false if it was absent.
    // Other native failures terminate the owning write transaction.
    bool del(std::uint32_t id);
    bool del(std::span<std::byte const> key);

    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;
    std::optional<std::span<std::byte const>> get(std::span<std::byte const> key) const;

    Result<> clear();

    KeyKind kind() const noexcept { return _kind; }

  private:
    Writer(DbiHandle dbi, WriteTransaction& txn, KeyKind kind);

    // After commit LMDB has closed every cursor; reuse is a caller precondition
    // violation and terminates before dereferencing the dangling handle.
    void ensureActive() const;
    void releaseFinishedCursor() noexcept;

    DbiHandle _dbi;
    WriteTransaction* _txn;
    Reader::CursorPtr _cursorPtr;
    std::uint32_t _lastId = 0; // Start from 1 (0 = null, so first append returns 1)
    KeyKind _kind;

    friend class Database;
  };
} // namespace ao::lmdb
