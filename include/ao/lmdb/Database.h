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
#include <utility>

// LMDB native handles, kept opaque (see Environment.h).
struct MDB_cursor;
struct MDB_txn;

namespace ao::lmdb
{
  namespace detail
  {
    class DatabaseAccess;
    class ReservationWriterAccess;

    // Read-only transactions leave cursor disposal to the caller. Write
    // transactions track every cursor and dispose of them at commit or abort.
    enum class CursorCleanup : std::uint8_t
    {
      Explicit,
      WriteTransaction
    };
  } // namespace detail

  class ReadTransaction;
  class WriteTransaction;

  /**
   * IntegerKeyDatabase - An LMDB DBI with native uint32 integer keys.
   */
  class IntegerKeyDatabase final
  {
  public:
    class Reader;
    class Writer;

    static Result<IntegerKeyDatabase> open(WriteTransaction& transaction, std::string const& name);
    static Result<IntegerKeyDatabase> openExisting(WriteTransaction& transaction, std::string const& name);

    Reader reader(ReadTransaction const& transaction) const;
    Writer writer(WriteTransaction& transaction) const;

  private:
    explicit IntegerKeyDatabase(DbiHandle dbi) noexcept
      : _dbi{dbi}
    {
    }

    DbiHandle _dbi = std::numeric_limits<DbiHandle>::max();

    friend class detail::DatabaseAccess;
  };

  /**
   * ByteKeyDatabase - An LMDB DBI with non-empty byte-string keys.
   */
  class ByteKeyDatabase final
  {
  public:
    class Reader;
    class Writer;

    static Result<ByteKeyDatabase> open(WriteTransaction& transaction, std::string const& name);
    static Result<ByteKeyDatabase> openExisting(WriteTransaction& transaction, std::string const& name);
    static Result<ByteKeyDatabase> main(WriteTransaction& transaction);

    Reader reader(ReadTransaction const& transaction) const;
    Writer writer(WriteTransaction& transaction) const;

  private:
    explicit ByteKeyDatabase(DbiHandle dbi) noexcept
      : _dbi{dbi}
    {
    }

    DbiHandle _dbi = std::numeric_limits<DbiHandle>::max();

    friend class detail::DatabaseAccess;
  };

  /**
   * IntegerKeyDatabase::Reader - Read-only integer-key access within a transaction.
   *
   * The referenced transaction must remain active while an operation or
   * iterator access is in progress. Reader and iterator destruction remains
   * safe after the transaction ends.
   */
  class IntegerKeyDatabase::Reader final
  {
  public:
    class Iterator;

    /**
     * KeyView - Raw integer-key bytes retained for storage-integrity checks.
     */
    class KeyView final
    {
    public:
      ~KeyView() = default;
      KeyView(KeyView const&) = default;
      KeyView& operator=(KeyView const&) = default;
      KeyView(KeyView&&) = default;
      KeyView& operator=(KeyView&&) = default;

      std::byte const* data() const noexcept { return _bytes.data(); }
      std::size_t size() const noexcept { return _bytes.size(); }
      bool empty() const noexcept { return _bytes.empty(); }
      auto begin() const noexcept { return _bytes.begin(); }
      auto end() const noexcept { return _bytes.end(); }
      std::span<std::byte const> bytes() const noexcept { return _bytes; }

      explicit operator std::uint32_t() const;

    private:
      explicit KeyView(std::span<std::byte const> bytes) noexcept
        : _bytes{bytes}
      {
      }

      std::span<std::byte const> _bytes;

      friend class Iterator;
    };

    struct EndSentinel
    {};
    using Value = std::pair<KeyView, std::span<std::byte const>>;

    Iterator begin() const;
    EndSentinel end() const { return {}; }

    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;
    std::size_t entryCount() const;
    std::uint32_t maxKey() const;

    ~Reader() = default;
    Reader(Reader const&) = default;
    Reader& operator=(Reader const&) = default;
    Reader(Reader&&) = default;
    Reader& operator=(Reader&&) = default;

  private:
    Reader(DbiHandle dbi, MDB_txn* transaction, ReadTransaction const& owner);

    struct MdbCursorDeleter final
    {
      detail::CursorCleanup cleanup = detail::CursorCleanup::Explicit;
      void operator()(MDB_cursor* cursor) const noexcept;
    };

    using CursorPtr = std::unique_ptr<MDB_cursor, MdbCursorDeleter>;
    static CursorPtr create(MDB_txn* transaction, ReadTransaction const& owner, DbiHandle dbi);
    void ensureActive() const;

    DbiHandle _dbi;
    MDB_txn* _txn;
    ReadTransaction const* _owner;

    friend class IntegerKeyDatabase;
    friend class Writer;
  };

  class IntegerKeyDatabase::Reader::Iterator final
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

    Iterator(Iterator const&) = delete;
    Iterator& operator=(Iterator const&) = delete;

    reference operator*() const;
    pointer operator->() const;

    Iterator& operator++();
    void operator++(std::int32_t) { ++*this; }
    bool operator==(Iterator const& other) const;
    bool operator==(EndSentinel /*unused*/) const { return *this == Iterator{}; }

  private:
    Iterator(MDB_txn* transaction, ReadTransaction const& owner, DbiHandle dbi);

    void ensureActive() const;
    void next();

    Reader::CursorPtr _cursorPtr;
    Reader::Value _value{Reader::KeyView{std::span<std::byte const>{}}, std::span<std::byte const>{}};
    ReadTransaction const* _owner = nullptr;

    friend class Reader;
  };

  /**
   * ByteKeyDatabase::Reader - Read-only byte-key access within a transaction.
   *
   * The referenced transaction must remain active while an operation or
   * iterator access is in progress. Reader and iterator destruction remains
   * safe after the transaction ends.
   */
  class ByteKeyDatabase::Reader final
  {
  public:
    using KeyView = std::span<std::byte const>;
    struct EndSentinel
    {};
    using Value = std::pair<KeyView, std::span<std::byte const>>;
    class Iterator;

    Iterator begin() const;
    /**
     * Returns the first record whose key is not less than `key`.
     *
     * `key` must not be empty; use `begin()` to seek the first record.
     */
    Iterator lowerBound(std::span<std::byte const> key) const;
    EndSentinel end() const { return {}; }

    std::optional<std::span<std::byte const>> get(std::span<std::byte const> key) const;
    std::size_t entryCount() const;

    ~Reader() = default;
    Reader(Reader const&) = default;
    Reader& operator=(Reader const&) = default;
    Reader(Reader&&) = default;
    Reader& operator=(Reader&&) = default;

  private:
    Reader(DbiHandle dbi, MDB_txn* transaction, ReadTransaction const& owner);

    struct MdbCursorDeleter final
    {
      detail::CursorCleanup cleanup = detail::CursorCleanup::Explicit;
      void operator()(MDB_cursor* cursor) const noexcept;
    };

    using CursorPtr = std::unique_ptr<MDB_cursor, MdbCursorDeleter>;
    static CursorPtr create(MDB_txn* transaction, ReadTransaction const& owner, DbiHandle dbi);
    void ensureActive() const;

    DbiHandle _dbi;
    MDB_txn* _txn;
    ReadTransaction const* _owner;

    friend class ByteKeyDatabase;
    friend class Writer;
  };

  class ByteKeyDatabase::Reader::Iterator final
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

    Iterator(Iterator const&) = delete;
    Iterator& operator=(Iterator const&) = delete;

    reference operator*() const;
    pointer operator->() const;

    Iterator& operator++();
    void operator++(std::int32_t) { ++*this; }
    bool operator==(Iterator const& other) const;
    bool operator==(EndSentinel /*unused*/) const { return *this == Iterator{}; }

  private:
    Iterator(MDB_txn* transaction, ReadTransaction const& owner, DbiHandle dbi);
    Iterator(MDB_txn* transaction,
             ReadTransaction const& owner,
             DbiHandle dbi,
             std::span<std::byte const> lowerBoundKey);

    void ensureActive() const;
    void next();

    Reader::CursorPtr _cursorPtr;
    Reader::Value _value;
    ReadTransaction const* _owner = nullptr;

    friend class Reader;
  };

  /**
   * IntegerKeyDatabase::Writer - Integer-key write access within a transaction.
   *
   * The referenced transaction must remain active while an operation is in
   * progress. Writer destruction remains safe after the transaction ends.
   */
  class [[nodiscard]] IntegerKeyDatabase::Writer final
  {
  public:
    ~Writer() noexcept;

    Writer(Writer const&) = delete;
    Writer& operator=(Writer const&) = delete;
    Writer(Writer&& other) noexcept;
    Writer& operator=(Writer&& other) noexcept;

    Result<> create(std::uint32_t id, std::span<std::byte const> data);

    std::uint32_t maxKey() const noexcept { return _lastId; }
    Result<std::uint32_t> append(std::span<std::byte const> data);

    Result<> update(std::uint32_t id, std::span<std::byte const> data);

    bool del(std::uint32_t id);
    std::optional<std::span<std::byte const>> get(std::uint32_t id) const;
    Result<> clear();

  private:
    Writer(DbiHandle dbi, WriteTransaction& transaction);

    void ensureActive() const;

    Result<std::span<std::byte>> reserveCreate(std::uint32_t id, std::size_t size);
    Result<std::pair<std::uint32_t, std::span<std::byte>>> reserveAppend(std::size_t size);
    Result<std::span<std::byte>> reserveUpdate(std::uint32_t id, std::size_t size);

    DbiHandle _dbi;
    WriteTransaction* _txn;
    Reader::CursorPtr _cursorPtr;
    std::uint32_t _lastId = 0;

    friend class IntegerKeyDatabase;
    friend class detail::ReservationWriterAccess;
  };

  /**
   * ByteKeyDatabase::Writer - Copied-value byte-key write access within a transaction.
   *
   * The referenced transaction must remain active while an operation is in
   * progress. Writer destruction remains safe after the transaction ends.
   */
  class [[nodiscard]] ByteKeyDatabase::Writer final
  {
  public:
    ~Writer() noexcept;

    Writer(Writer const&) = delete;
    Writer& operator=(Writer const&) = delete;
    Writer(Writer&& other) noexcept;
    Writer& operator=(Writer&& other) noexcept;

    Result<> create(std::span<std::byte const> key, std::span<std::byte const> data);
    Result<> update(std::span<std::byte const> key, std::span<std::byte const> data);
    bool del(std::span<std::byte const> key);
    std::optional<std::span<std::byte const>> get(std::span<std::byte const> key) const;
    Result<> clear();

  private:
    Writer(DbiHandle dbi, WriteTransaction& transaction);

    void ensureActive() const;

    DbiHandle _dbi;
    WriteTransaction* _txn;
    Reader::CursorPtr _cursorPtr;

    friend class ByteKeyDatabase;
  };
} // namespace ao::lmdb
