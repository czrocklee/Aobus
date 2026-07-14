// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

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
    static Result<Database> open(ReadTransaction& txn, std::string const& name, KeyKind kind = KeyKind::Integer);

    Reader reader(ReadTransaction const& txn) const;
    Writer writer(WriteTransaction& txn) const;

    KeyKind kind() const noexcept { return _kind; }

  private:
    Database(DbiHandle dbi, KeyKind kind);

    DbiHandle _dbi = std::numeric_limits<DbiHandle>::max();
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
      // Coerce an integer key to uint32. Throws on a size mismatch rather than
      // silently yielding 0, so a corrupt/non-integer key surfaces as an error
      // instead of a bogus id (release builds strip gsl contracts, so this guard
      // must be a plain throw).
      operator std::uint32_t() const;
    };

    struct EndSentinel
    {};
    using Value = std::pair<KeyView, std::span<std::byte const>>;
    class Iterator;

    Iterator begin() const;
    EndSentinel end() const { return {}; }

    // Point lookups. A missing key is the only recoverable outcome and is
    // reported as std::nullopt; non-NotFound storage faults throw (see the
    // Iterator note below for why corruption is fatal at this layer).
    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;
    std::optional<std::span<std::byte const>> get(std::span<std::byte const> key) const;

    // Number of rows visible to this transaction. Throws on fault.
    std::size_t entryCount() const;

    // Largest integer key, or 0 when the database is empty. Throws on fault.
    std::uint32_t maxKey() const;

    ~Reader() = default;

    // copyable and movable
    Reader(Reader const&) = default;
    Reader& operator=(Reader const&) = default;
    Reader(Reader&&) = default;
    Reader& operator=(Reader&&) = default;

    KeyKind kind() const noexcept { return _kind; }

  private:
    Reader(DbiHandle dbi, MDB_txn* txn, KeyKind kind);

    struct MdbCursorDeleter final
    {
      void operator()(MDB_cursor* cur) const noexcept;
    };

    using CursorPtr = std::unique_ptr<MDB_cursor, MdbCursorDeleter>;
    static CursorPtr create(MDB_txn* txn, DbiHandle dbi);

    DbiHandle _dbi;
    MDB_txn* _txn;
    KeyKind _kind;

    friend class Database;
    friend class Writer;
  };

  /**
   * Database::Reader::Iterator - Input iterator over database entries.
   *
   * Cursor EOF (MDB_NOTFOUND) is normal and compares equal to EndSentinel.
   * Any other cursor failure throws. These are either programmer errors
   * (EINVAL) or unrecoverable storage faults (MDB_PANIC, MDB_CORRUPTED): the
   * same on-disk corruption that yields MDB_CORRUPTED here equally surfaces as
   * SIGBUS through the mmap, which no Result can intercept, so a recoverable
   * channel at this layer would be a false promise. We therefore treat every
   * non-EOF cursor failure as fatal, consistent with the error model's
   * invariant/fatal rule. The same reasoning is why point lookups and metadata
   * queries (get/entryCount/maxKey) report only the recoverable miss and throw
   * on everything else.
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
    Iterator(MDB_txn* txn, DbiHandle dbi, bool end);

    void next();

    Reader::CursorPtr _cursorPtr;
    Reader::Value _value;

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
    Writer(Writer&&) noexcept = default;
    Writer& operator=(Writer&&) noexcept = default;

    Result<> create(std::uint32_t id, std::span<std::byte const> data);
    Result<> create(std::span<std::byte const> key, std::span<std::byte const> data);

    Result<std::span<std::byte>> create(std::uint32_t id, std::size_t size);
    Result<std::span<std::byte>> create(std::span<std::byte const> key, std::size_t size);

    std::uint32_t maxKey() const noexcept { return _lastId; }
    Result<std::uint32_t> append(std::span<std::byte const> data);
    Result<std::pair<std::uint32_t, std::span<std::byte>>> append(std::size_t size);

    Result<> update(std::uint32_t id, std::span<std::byte const> data);
    Result<> update(std::span<std::byte const> key, std::span<std::byte const> data);

    Result<std::span<std::byte>> update(std::uint32_t id, std::size_t size);
    Result<std::span<std::byte>> update(std::span<std::byte const> key, std::size_t size);

    // Delete a key. Returns true if a row was removed, false if the key was
    // absent (idempotent callers can ignore the result). Throws on fault.
    bool del(std::uint32_t id);
    bool del(std::span<std::byte const> key);

    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;
    std::optional<std::span<std::byte const>> get(std::span<std::byte const> key) const;

    Result<> clear();

    KeyKind kind() const noexcept { return _kind; }

  private:
    Writer(DbiHandle dbi, WriteTransaction& txn, KeyKind kind);

    // Throw if the owning transaction has already been committed. After commit
    // LMDB has closed every cursor, so reusing this writer would dereference a
    // dangling cursor. Always-on (not gsl-gated) since release strips contracts.
    void ensureActive() const;

    DbiHandle _dbi;
    WriteTransaction* _txn;
    Reader::CursorPtr _cursorPtr;
    std::uint32_t _lastId = 0; // Start from 1 (0 = null, so first append returns 1)
    KeyKind _kind;

    friend class Database;
  };
} // namespace ao::lmdb
