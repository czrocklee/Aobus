// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/lmdb/Database.h>

#include "detail/ResultError.h"
#include "detail/ThrowError.h"
#include "detail/UnvalidatedDatabase.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <lmdb.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::lmdb
{
  namespace detail
  {
    class DatabaseAccess final
    {
    public:
      static MDB_txn* handle(ReadTransaction const& transaction) noexcept { return transaction._txnPtr.get(); }

      static std::uint64_t bindingGeneration(ReadTransaction const& transaction) noexcept
      {
        return transaction._bindingGeneration;
      }

      static bool isTransactionOwned(ReadTransaction const& transaction) noexcept
      {
        return transaction._failureMode == ReadTransaction::ReadFailureMode::Transaction;
      }

      static std::size_t* openCursorCount(ReadTransaction const& transaction) noexcept
      {
        return transaction._openCursorCountPtr.get();
      }

      static void acquireOpenAdmission(WriteTransaction& transaction) { transaction.acquireDatabaseOpenAdmission(); }

      // Readonly owners have no affinity under MDB_NOTLS; writer capabilities do.
      static bool isOnOwnerThread(ReadTransaction const& transaction) noexcept { return transaction.isOwnerThread(); }

      static IntegerKeyDatabase integerKeyDatabase(DbiHandle const dbi) noexcept { return IntegerKeyDatabase{dbi}; }
    };
  } // namespace detail

  namespace
  {
    constexpr auto kInvalidDbi = std::numeric_limits<DbiHandle>::max();

    ::MDB_val makeVal(std::span<std::byte const> const bytes)
    {
      return {.mv_size = bytes.size(), .mv_data = utility::layout::asLegacyPtr<void>(bytes.data())};
    }

    std::span<std::byte const> viewOf(::MDB_val const value)
    {
      return utility::bytes::view(static_cast<void const*>(value.mv_data), value.mv_size);
    }

    std::uint32_t readIntegerKey(std::span<std::byte const> const key)
    {
      std::uint32_t value = 0;
      AO_INVARIANT(key.size() == sizeof(value), "LMDB integer key has an invalid size");
      std::memcpy(&value, key.data(), sizeof(value));
      return value;
    }

    struct OpenedDatabase final
    {
      DbiHandle dbi = 0;
      std::uint32_t nativeFlags = 0;
    };

    std::uint32_t readDatabaseFlags(MDB_txn* transaction, DbiHandle const dbi)
    {
      unsigned int flags = 0;

      if (auto const code = ::mdb_dbi_flags(transaction, dbi, &flags); code != MDB_SUCCESS)
      {
        throwOnMutationError("mdb_dbi_flags", code);
      }

      return flags;
    }

    Result<OpenedDatabase> openNamedDatabase(WriteTransaction& owner,
                                             std::string const& name,
                                             std::uint32_t const openFlags)
    {
      AO_EXPECTS(owner.isActive(), "Cannot open a database with a finished write transaction");
      detail::DatabaseAccess::acquireOpenAdmission(owner);
      auto* const transaction = detail::DatabaseAccess::handle(owner);
      DbiHandle dbi = 0;
      auto const code = ::mdb_dbi_open(transaction, name.c_str(), openFlags, &dbi);

      if (code == MDB_NOTFOUND)
      {
        return makeError(Error::Code::NotFound, std::format("Named database '{}' does not exist", name));
      }

      if (code == MDB_INCOMPATIBLE)
      {
        return makeError(
          Error::Code::CorruptData, std::format("Main catalog entry '{}' is not a named database", name));
      }

      if (code != MDB_SUCCESS)
      {
        throwOnMutationError("mdb_dbi_open", code);
      }

      return OpenedDatabase{.dbi = dbi, .nativeFlags = readDatabaseFlags(transaction, dbi)};
    }

    Result<> validateExactFlags(std::string_view const databaseName,
                                std::uint32_t const nativeFlags,
                                std::uint32_t const expectedFlags)
    {
      if (nativeFlags != expectedFlags)
      {
        return makeError(
          Error::Code::CorruptData,
          std::format(
            "Named database '{}' has flags 0x{:x} (expected 0x{:x})", databaseName, nativeFlags, expectedFlags));
      }

      return {};
    }

    enum class NamedOpen : std::uint8_t
    {
      Existing,
      Create
    };

    Result<DbiHandle> openTypedDatabase(WriteTransaction& transaction,
                                        std::string const& name,
                                        NamedOpen const mode,
                                        std::uint32_t const expectedFlags)
    {
      auto const openFlags = mode == NamedOpen::Create ? (MDB_CREATE | expectedFlags) : 0U;
      auto openedRes = openNamedDatabase(transaction, name, openFlags);

      if (!openedRes)
      {
        return std::unexpected{std::move(openedRes.error())};
      }

      if (auto validationRes = validateExactFlags(name, openedRes->nativeFlags, expectedFlags); !validationRes)
      {
        return std::unexpected{std::move(validationRes.error())};
      }

      return openedRes->dbi;
    }

    Result<> validateByteKeyMain(std::uint32_t const nativeFlags)
    {
      if (nativeFlags != 0)
      {
        return makeError(
          Error::Code::CorruptData, std::format("Main database has flags 0x{:x} (expected 0x0)", nativeFlags));
      }

      return {};
    }

    std::optional<std::span<std::byte const>> readPoint(MDB_txn* transaction,
                                                        DbiHandle const dbi,
                                                        std::span<std::byte const> const keyView,
                                                        bool const transactionOwned)
    {
      auto key = makeVal(keyView);
      auto value = ::MDB_val{};
      auto const code = ::mdb_get(transaction, dbi, &key, &value);

      if (code == MDB_NOTFOUND)
      {
        return std::nullopt;
      }

      failRead("mdb_get", code, transactionOwned);
      return viewOf(value);
    }

    std::size_t readEntryCount(MDB_txn* transaction, DbiHandle const dbi, bool const transactionOwned)
    {
      auto stat = ::MDB_stat{};
      failRead("mdb_stat", ::mdb_stat(transaction, dbi, &stat), transactionOwned);
      return stat.ms_entries;
    }

    MDB_cursor* openCursor(MDB_txn* transaction, DbiHandle const dbi, bool const transactionOwned)
    {
      MDB_cursor* cursor = nullptr;
      failRead("mdb_cursor_open", ::mdb_cursor_open(transaction, dbi, &cursor), transactionOwned);
      return cursor;
    }

    struct RawRecord final
    {
      std::span<std::byte const> key;
      std::span<std::byte const> value;
    };

    std::optional<RawRecord> positionCursor(MDB_cursor* cursor,
                                            MDB_cursor_op const operation,
                                            bool const transactionOwned,
                                            ::MDB_val key = {})
    {
      auto value = ::MDB_val{};
      auto const code = ::mdb_cursor_get(cursor, &key, &value, operation);

      if (code == MDB_NOTFOUND)
      {
        return std::nullopt;
      }

      failRead("mdb_cursor_get", code, transactionOwned);
      return RawRecord{.key = viewOf(key), .value = viewOf(value)};
    }

    Result<> put(MDB_cursor* cursor,
                 std::span<std::byte const> const keyView,
                 ::MDB_val& value,
                 unsigned int const flags)
    {
      AO_INVARIANT(cursor != nullptr);
      auto key = makeVal(keyView);
      auto const code = ::mdb_cursor_put(cursor, &key, &value, flags);

      if (code != MDB_SUCCESS && (code != MDB_KEYEXIST || (flags & MDB_NOOVERWRITE) == 0))
      {
        throwOnMutationError("mdb_cursor_put", code);
      }

      return resultFromCode("mdb_cursor_put", code);
    }

    Result<> putCopied(MDB_cursor* cursor,
                       std::span<std::byte const> const key,
                       std::span<std::byte const> const data,
                       unsigned int const flags)
    {
      auto value = makeVal(data);
      return put(cursor, key, value, flags);
    }

    Result<std::span<std::byte>> reserve(MDB_cursor* cursor,
                                         std::span<std::byte const> const key,
                                         std::size_t const size,
                                         unsigned int const flags)
    {
      auto value = ::MDB_val{.mv_size = size, .mv_data = nullptr};

      if (auto res = put(cursor, key, value, flags | MDB_RESERVE); !res)
      {
        return std::unexpected{std::move(res.error())};
      }

      return utility::bytes::view(value.mv_data, value.mv_size);
    }

    bool tryDeleteKey(MDB_cursor* cursor, std::span<std::byte const> const keyView)
    {
      auto key = makeVal(keyView);
      auto const code = ::mdb_cursor_get(cursor, &key, nullptr, MDB_SET);

      if (code == MDB_NOTFOUND)
      {
        return false;
      }

      if (code != MDB_SUCCESS)
      {
        throwOnMutationError("mdb_cursor_get", code);
      }

      if (auto const deleteCode = ::mdb_cursor_del(cursor, 0); deleteCode != MDB_SUCCESS)
      {
        throwOnMutationError("mdb_cursor_del", deleteCode);
      }

      return true;
    }

    std::optional<std::span<std::byte const>> readCursorPoint(MDB_cursor* cursor,
                                                              std::span<std::byte const> const keyView)
    {
      return positionCursor(cursor, MDB_SET, true, makeVal(keyView))
        .transform([](RawRecord const& record) { return record.value; });
    }

    void clearDatabase(MDB_txn* transaction, DbiHandle const dbi)
    {
      if (auto const code = ::mdb_drop(transaction, dbi, 0); code != MDB_SUCCESS)
      {
        throwOnMutationError("mdb_drop", code);
      }
    }
  } // namespace

  Result<IntegerKeyDatabase> IntegerKeyDatabase::open(WriteTransaction& transaction, std::string const& name)
  {
    return openTypedDatabase(transaction, name, NamedOpen::Create, MDB_INTEGERKEY)
      .transform([](DbiHandle const dbi) { return IntegerKeyDatabase{dbi}; });
  }

  Result<IntegerKeyDatabase> IntegerKeyDatabase::openExisting(WriteTransaction& transaction, std::string const& name)
  {
    return openTypedDatabase(transaction, name, NamedOpen::Existing, MDB_INTEGERKEY)
      .transform([](DbiHandle const dbi) { return IntegerKeyDatabase{dbi}; });
  }

  Result<ByteKeyDatabase> ByteKeyDatabase::open(WriteTransaction& transaction, std::string const& name)
  {
    return openTypedDatabase(transaction, name, NamedOpen::Create, 0)
      .transform([](DbiHandle const dbi) { return ByteKeyDatabase{dbi}; });
  }

  Result<ByteKeyDatabase> ByteKeyDatabase::openExisting(WriteTransaction& transaction, std::string const& name)
  {
    return openTypedDatabase(transaction, name, NamedOpen::Existing, 0)
      .transform([](DbiHandle const dbi) { return ByteKeyDatabase{dbi}; });
  }

  Result<ByteKeyDatabase> ByteKeyDatabase::main(WriteTransaction& transaction)
  {
    AO_EXPECTS(transaction.isActive(), "Cannot access the main database with a finished write transaction");
    detail::DatabaseAccess::acquireOpenAdmission(transaction);
    DbiHandle dbi = 0;
    auto const code = ::mdb_dbi_open(detail::DatabaseAccess::handle(transaction), nullptr, 0, &dbi);

    if (code != MDB_SUCCESS)
    {
      throwOnMutationError("mdb_dbi_open", code);
    }

    auto const flags = readDatabaseFlags(detail::DatabaseAccess::handle(transaction), dbi);

    if (auto validationRes = validateByteKeyMain(flags); !validationRes)
    {
      return std::unexpected{std::move(validationRes.error())};
    }

    return ByteKeyDatabase{dbi};
  }

  IntegerKeyDatabase::Reader IntegerKeyDatabase::reader(ReadTransaction const& transaction) const
  {
    AO_EXPECTS(transaction.isActive(), "IntegerKeyDatabase::Reader created from an inactive transaction");
    return Reader{_dbi, detail::DatabaseAccess::handle(transaction), transaction};
  }

  IntegerKeyDatabase::Writer IntegerKeyDatabase::writer(WriteTransaction& transaction) const
  {
    return Writer{_dbi, transaction};
  }

  ByteKeyDatabase::Reader ByteKeyDatabase::reader(ReadTransaction const& transaction) const
  {
    AO_EXPECTS(transaction.isActive(), "ByteKeyDatabase::Reader created from an inactive transaction");
    return Reader{_dbi, detail::DatabaseAccess::handle(transaction), transaction};
  }

  ByteKeyDatabase::Writer ByteKeyDatabase::writer(WriteTransaction& transaction) const
  {
    return Writer{_dbi, transaction};
  }

  IntegerKeyDatabase::Reader::Reader(DbiHandle const dbi, MDB_txn* transaction, ReadTransaction const& owner)
    : _dbi{dbi}, _txn{transaction}, _owner{&owner}, _bindingGeneration{detail::DatabaseAccess::bindingGeneration(owner)}
  {
  }

  IntegerKeyDatabase::Reader::Iterator IntegerKeyDatabase::Reader::begin() const
  {
    ensureActive();
    return Iterator{_txn, *_owner, _dbi};
  }

  std::optional<std::span<std::byte const>> IntegerKeyDatabase::Reader::get(std::uint32_t const id) const
  {
    ensureActive();
    return readPoint(_txn, _dbi, utility::bytes::view(id), detail::DatabaseAccess::isTransactionOwned(*_owner));
  }

  std::size_t IntegerKeyDatabase::Reader::entryCount() const
  {
    ensureActive();
    return readEntryCount(_txn, _dbi, detail::DatabaseAccess::isTransactionOwned(*_owner));
  }

  std::uint32_t IntegerKeyDatabase::Reader::maxKey() const
  {
    ensureActive();
    auto cursorPtr = create(_txn, *_owner, _dbi);
    auto const optRecord =
      positionCursor(cursorPtr.get(), MDB_LAST, detail::DatabaseAccess::isTransactionOwned(*_owner));
    return optRecord ? readIntegerKey(optRecord->key) : 0;
  }

  void IntegerKeyDatabase::Reader::MdbCursorDeleter::operator()(MDB_cursor* cursor) const noexcept
  {
    if (openCursorCount != nullptr)
    {
      ::mdb_cursor_close(cursor);
      --*openCursorCount;
    }
  }

  IntegerKeyDatabase::Reader::CursorPtr IntegerKeyDatabase::Reader::create(MDB_txn* transaction,
                                                                           ReadTransaction const& owner,
                                                                           DbiHandle const dbi)
  {
    auto* const cursor = openCursor(transaction, dbi, detail::DatabaseAccess::isTransactionOwned(owner));
    auto* const openCursorCount = detail::DatabaseAccess::openCursorCount(owner);

    if (openCursorCount != nullptr)
    {
      ++*openCursorCount;
    }

    return CursorPtr{cursor, MdbCursorDeleter{.openCursorCount = openCursorCount}};
  }

  void IntegerKeyDatabase::Reader::ensureActive() const
  {
    AO_EXPECTS(_owner != nullptr && _owner->isActive() &&
                 detail::DatabaseAccess::bindingGeneration(*_owner) == _bindingGeneration &&
                 detail::DatabaseAccess::handle(*_owner) == _txn,
               "IntegerKeyDatabase::Reader used after its transaction finished or was replaced");
    AO_EXPECTS(detail::DatabaseAccess::isOnOwnerThread(*_owner),
               "IntegerKeyDatabase::Reader used off its write transaction's creating thread");
  }

  IntegerKeyDatabase::Reader::KeyView::operator std::uint32_t() const
  {
    AO_INVARIANT(size() == sizeof(std::uint32_t), "LMDB integer key has an invalid size");

    std::uint32_t value = 0;
    std::memcpy(&value, data(), sizeof(value));
    return value;
  }

  IntegerKeyDatabase::Reader::Iterator::Iterator(MDB_txn* transaction,
                                                 ReadTransaction const& owner,
                                                 DbiHandle const dbi)
    : _cursorPtr{Reader::create(transaction, owner, dbi)}
    , _txn{transaction}
    , _owner{&owner}
    , _bindingGeneration{detail::DatabaseAccess::bindingGeneration(owner)}
  {
    auto const optRecord =
      positionCursor(_cursorPtr.get(), MDB_FIRST, detail::DatabaseAccess::isTransactionOwned(owner));

    if (optRecord)
    {
      _value = Reader::Value{Reader::KeyView{optRecord->key}, optRecord->value};
    }
    else
    {
      _cursorPtr.reset();
    }
  }

  IntegerKeyDatabase::Reader::Iterator::Iterator(Iterator&& other) noexcept
    : _cursorPtr{std::move(other._cursorPtr)}
    , _value{other._value}
    , _txn{std::exchange(other._txn, nullptr)}
    , _owner{std::exchange(other._owner, nullptr)}
    , _bindingGeneration{std::exchange(other._bindingGeneration, 0)}
  {
    other._value = Reader::Value{Reader::KeyView{std::span<std::byte const>{}}, std::span<std::byte const>{}};
  }

  IntegerKeyDatabase::Reader::Iterator& IntegerKeyDatabase::Reader::Iterator::operator=(Iterator&& other) noexcept
  {
    if (this == &other)
    {
      return *this;
    }

    _cursorPtr = std::move(other._cursorPtr);
    _value = other._value;
    _txn = std::exchange(other._txn, nullptr);
    _owner = std::exchange(other._owner, nullptr);
    _bindingGeneration = std::exchange(other._bindingGeneration, 0);
    other._value = Reader::Value{Reader::KeyView{std::span<std::byte const>{}}, std::span<std::byte const>{}};
    return *this;
  }

  IntegerKeyDatabase::Reader::Iterator::~Iterator() noexcept = default;

  IntegerKeyDatabase::Reader::Iterator::reference IntegerKeyDatabase::Reader::Iterator::operator*() const
  {
    ensureActive();
    AO_EXPECTS(_cursorPtr != nullptr);
    return _value;
  }

  IntegerKeyDatabase::Reader::Iterator::pointer IntegerKeyDatabase::Reader::Iterator::operator->() const
  {
    ensureActive();
    AO_EXPECTS(_cursorPtr != nullptr);
    return &_value;
  }

  IntegerKeyDatabase::Reader::Iterator& IntegerKeyDatabase::Reader::Iterator::operator++()
  {
    ensureActive();
    AO_EXPECTS(_cursorPtr != nullptr);
    next();
    return *this;
  }

  bool IntegerKeyDatabase::Reader::Iterator::operator==(Iterator const& other) const
  {
    return _cursorPtr == other._cursorPtr;
  }

  void IntegerKeyDatabase::Reader::Iterator::ensureActive() const
  {
    AO_EXPECTS(_owner != nullptr && _owner->isActive() &&
                 detail::DatabaseAccess::bindingGeneration(*_owner) == _bindingGeneration &&
                 detail::DatabaseAccess::handle(*_owner) == _txn,
               "IntegerKeyDatabase::Reader::Iterator used after its transaction finished or was replaced");
    AO_EXPECTS(detail::DatabaseAccess::isOnOwnerThread(*_owner),
               "IntegerKeyDatabase::Reader::Iterator used off its write transaction's creating thread");
  }

  void IntegerKeyDatabase::Reader::Iterator::next()
  {
    auto const optRecord =
      positionCursor(_cursorPtr.get(), MDB_NEXT, detail::DatabaseAccess::isTransactionOwned(*_owner));

    if (optRecord)
    {
      _value = Reader::Value{Reader::KeyView{optRecord->key}, optRecord->value};
    }
    else
    {
      _value = Reader::Value{Reader::KeyView{std::span<std::byte const>{}}, std::span<std::byte const>{}};
      _cursorPtr.reset();
    }
  }

  ByteKeyDatabase::Reader::Reader(DbiHandle const dbi, MDB_txn* transaction, ReadTransaction const& owner)
    : _dbi{dbi}, _txn{transaction}, _owner{&owner}, _bindingGeneration{detail::DatabaseAccess::bindingGeneration(owner)}
  {
  }

  ByteKeyDatabase::Reader::Iterator ByteKeyDatabase::Reader::begin() const
  {
    ensureActive();
    return Iterator{_txn, *_owner, _dbi};
  }

  ByteKeyDatabase::Reader::Iterator ByteKeyDatabase::Reader::lowerBound(std::span<std::byte const> const key) const
  {
    ensureActive();
    AO_EXPECTS(!key.empty(), "ByteKeyDatabase::Reader::lowerBound requires a non-empty key");
    return Iterator{_txn, *_owner, _dbi, key};
  }

  std::optional<std::span<std::byte const>> ByteKeyDatabase::Reader::get(std::span<std::byte const> const key) const
  {
    ensureActive();
    return readPoint(_txn, _dbi, key, detail::DatabaseAccess::isTransactionOwned(*_owner));
  }

  std::size_t ByteKeyDatabase::Reader::entryCount() const
  {
    ensureActive();
    return readEntryCount(_txn, _dbi, detail::DatabaseAccess::isTransactionOwned(*_owner));
  }

  void ByteKeyDatabase::Reader::MdbCursorDeleter::operator()(MDB_cursor* cursor) const noexcept
  {
    if (openCursorCount != nullptr)
    {
      ::mdb_cursor_close(cursor);
      --*openCursorCount;
    }
  }

  ByteKeyDatabase::Reader::CursorPtr ByteKeyDatabase::Reader::create(MDB_txn* transaction,
                                                                     ReadTransaction const& owner,
                                                                     DbiHandle const dbi)
  {
    auto* const cursor = openCursor(transaction, dbi, detail::DatabaseAccess::isTransactionOwned(owner));
    auto* const openCursorCount = detail::DatabaseAccess::openCursorCount(owner);

    if (openCursorCount != nullptr)
    {
      ++*openCursorCount;
    }

    return CursorPtr{cursor, MdbCursorDeleter{.openCursorCount = openCursorCount}};
  }

  void ByteKeyDatabase::Reader::ensureActive() const
  {
    AO_EXPECTS(_owner != nullptr && _owner->isActive() &&
                 detail::DatabaseAccess::bindingGeneration(*_owner) == _bindingGeneration &&
                 detail::DatabaseAccess::handle(*_owner) == _txn,
               "ByteKeyDatabase::Reader used after its transaction finished or was replaced");
    AO_EXPECTS(detail::DatabaseAccess::isOnOwnerThread(*_owner),
               "ByteKeyDatabase::Reader used off its write transaction's creating thread");
  }

  ByteKeyDatabase::Reader::Iterator::Iterator(MDB_txn* transaction, ReadTransaction const& owner, DbiHandle const dbi)
    : _cursorPtr{Reader::create(transaction, owner, dbi)}
    , _txn{transaction}
    , _owner{&owner}
    , _bindingGeneration{detail::DatabaseAccess::bindingGeneration(owner)}
  {
    auto const optRecord =
      positionCursor(_cursorPtr.get(), MDB_FIRST, detail::DatabaseAccess::isTransactionOwned(owner));

    if (optRecord)
    {
      _value = Reader::Value{optRecord->key, optRecord->value};
    }
    else
    {
      _cursorPtr.reset();
    }
  }

  ByteKeyDatabase::Reader::Iterator::Iterator(MDB_txn* transaction,
                                              ReadTransaction const& owner,
                                              DbiHandle const dbi,
                                              std::span<std::byte const> const lowerBoundKey)
    : _cursorPtr{Reader::create(transaction, owner, dbi)}
    , _txn{transaction}
    , _owner{&owner}
    , _bindingGeneration{detail::DatabaseAccess::bindingGeneration(owner)}
  {
    auto const optRecord = positionCursor(
      _cursorPtr.get(), MDB_SET_RANGE, detail::DatabaseAccess::isTransactionOwned(owner), makeVal(lowerBoundKey));

    if (optRecord)
    {
      _value = Reader::Value{optRecord->key, optRecord->value};
    }
    else
    {
      _cursorPtr.reset();
    }
  }

  ByteKeyDatabase::Reader::Iterator::Iterator(Iterator&& other) noexcept
    : _cursorPtr{std::move(other._cursorPtr)}
    , _value{other._value}
    , _txn{std::exchange(other._txn, nullptr)}
    , _owner{std::exchange(other._owner, nullptr)}
    , _bindingGeneration{std::exchange(other._bindingGeneration, 0)}
  {
    other._value = {};
  }

  ByteKeyDatabase::Reader::Iterator& ByteKeyDatabase::Reader::Iterator::operator=(Iterator&& other) noexcept
  {
    if (this == &other)
    {
      return *this;
    }

    _cursorPtr = std::move(other._cursorPtr);
    _value = other._value;
    _txn = std::exchange(other._txn, nullptr);
    _owner = std::exchange(other._owner, nullptr);
    _bindingGeneration = std::exchange(other._bindingGeneration, 0);
    other._value = {};
    return *this;
  }

  ByteKeyDatabase::Reader::Iterator::~Iterator() noexcept = default;

  ByteKeyDatabase::Reader::Iterator::reference ByteKeyDatabase::Reader::Iterator::operator*() const
  {
    ensureActive();
    AO_EXPECTS(_cursorPtr != nullptr);
    return _value;
  }

  ByteKeyDatabase::Reader::Iterator::pointer ByteKeyDatabase::Reader::Iterator::operator->() const
  {
    ensureActive();
    AO_EXPECTS(_cursorPtr != nullptr);
    return &_value;
  }

  ByteKeyDatabase::Reader::Iterator& ByteKeyDatabase::Reader::Iterator::operator++()
  {
    ensureActive();
    AO_EXPECTS(_cursorPtr != nullptr);
    next();
    return *this;
  }

  bool ByteKeyDatabase::Reader::Iterator::operator==(Iterator const& other) const
  {
    return _cursorPtr == other._cursorPtr;
  }

  void ByteKeyDatabase::Reader::Iterator::ensureActive() const
  {
    AO_EXPECTS(_owner != nullptr && _owner->isActive() &&
                 detail::DatabaseAccess::bindingGeneration(*_owner) == _bindingGeneration &&
                 detail::DatabaseAccess::handle(*_owner) == _txn,
               "ByteKeyDatabase::Reader::Iterator used after its transaction finished or was replaced");
    AO_EXPECTS(detail::DatabaseAccess::isOnOwnerThread(*_owner),
               "ByteKeyDatabase::Reader::Iterator used off its write transaction's creating thread");
  }

  void ByteKeyDatabase::Reader::Iterator::next()
  {
    auto const optRecord =
      positionCursor(_cursorPtr.get(), MDB_NEXT, detail::DatabaseAccess::isTransactionOwned(*_owner));

    if (optRecord)
    {
      _value = Reader::Value{optRecord->key, optRecord->value};
    }
    else
    {
      _value = {};
      _cursorPtr.reset();
    }
  }

  IntegerKeyDatabase::Writer::Writer(DbiHandle const dbi, WriteTransaction& transaction)
    : _dbi{dbi}
    , _txn{&transaction}
    , _nativeTxn{detail::DatabaseAccess::handle(transaction)}
    , _bindingGeneration{detail::DatabaseAccess::bindingGeneration(transaction)}
  {
    ensureActive();
    _cursorPtr = Reader::create(detail::DatabaseAccess::handle(transaction), transaction, _dbi);

    if (auto const optRecord = positionCursor(_cursorPtr.get(), MDB_LAST, true); optRecord)
    {
      _lastId = readIntegerKey(optRecord->key);
    }
  }

  IntegerKeyDatabase::Writer::Writer(Writer&& other) noexcept
    : _dbi{other._dbi}
    , _txn{std::exchange(other._txn, nullptr)}
    , _nativeTxn{std::exchange(other._nativeTxn, nullptr)}
    , _bindingGeneration{std::exchange(other._bindingGeneration, 0)}
    , _cursorPtr{std::move(other._cursorPtr)}
    , _lastId{other._lastId}
  {
  }

  IntegerKeyDatabase::Writer& IntegerKeyDatabase::Writer::operator=(Writer&& other) noexcept
  {
    if (this == &other)
    {
      return *this;
    }

    _dbi = other._dbi;
    _txn = std::exchange(other._txn, nullptr);
    _nativeTxn = std::exchange(other._nativeTxn, nullptr);
    _bindingGeneration = std::exchange(other._bindingGeneration, 0);
    _cursorPtr = std::move(other._cursorPtr);
    _lastId = other._lastId;
    return *this;
  }

  IntegerKeyDatabase::Writer::~Writer() noexcept = default;

  void IntegerKeyDatabase::Writer::ensureActive() const
  {
    AO_EXPECTS(_txn != nullptr && _txn->isActive() &&
                 detail::DatabaseAccess::bindingGeneration(*_txn) == _bindingGeneration &&
                 detail::DatabaseAccess::handle(*_txn) == _nativeTxn,
               "IntegerKeyDatabase::Writer used after its transaction finished or was replaced");
    AO_EXPECTS(detail::DatabaseAccess::isOnOwnerThread(*_txn),
               "IntegerKeyDatabase::Writer used off its transaction's creating thread");
  }

  Result<> IntegerKeyDatabase::Writer::create(std::uint32_t const id, std::span<std::byte const> const data)
  {
    ensureActive();
    return putCopied(_cursorPtr.get(), utility::bytes::view(id), data, MDB_NOOVERWRITE);
  }

  Result<std::span<std::byte>> IntegerKeyDatabase::Writer::reserveCreate(std::uint32_t const id, std::size_t const size)
  {
    ensureActive();
    return reserve(_cursorPtr.get(), utility::bytes::view(id), size, MDB_NOOVERWRITE);
  }

  std::uint32_t IntegerKeyDatabase::Writer::maxKey() const noexcept
  {
    ensureActive();
    return _lastId;
  }

  void IntegerKeyDatabase::Writer::raiseMaxKey(std::uint32_t const key) noexcept
  {
    ensureActive();
    _lastId = std::max(_lastId, key);
  }

  Result<std::uint32_t> IntegerKeyDatabase::Writer::append(std::span<std::byte const> const data)
  {
    ensureActive();

    if (_lastId == std::numeric_limits<std::uint32_t>::max())
    {
      return makeError(Error::Code::ResourceExhausted, "LMDB integer key space exhausted");
    }

    auto const id = _lastId + 1;

    if (auto res = putCopied(_cursorPtr.get(), utility::bytes::view(id), data, MDB_NOOVERWRITE); !res)
    {
      return std::unexpected{std::move(res.error())};
    }

    _lastId = id;
    return id;
  }

  Result<std::pair<std::uint32_t, std::span<std::byte>>> IntegerKeyDatabase::Writer::reserveAppend(
    std::size_t const size)
  {
    ensureActive();

    if (_lastId == std::numeric_limits<std::uint32_t>::max())
    {
      return makeError(Error::Code::ResourceExhausted, "LMDB integer key space exhausted");
    }

    auto const id = _lastId + 1;
    auto dataRes = reserve(_cursorPtr.get(), utility::bytes::view(id), size, MDB_NOOVERWRITE);

    if (!dataRes)
    {
      return std::unexpected{std::move(dataRes.error())};
    }

    _lastId = id;
    return std::pair<std::uint32_t, std::span<std::byte>>{id, *dataRes};
  }

  Result<> IntegerKeyDatabase::Writer::update(std::uint32_t const id, std::span<std::byte const> const data)
  {
    ensureActive();
    return putCopied(_cursorPtr.get(), utility::bytes::view(id), data, 0);
  }

  Result<std::span<std::byte>> IntegerKeyDatabase::Writer::reserveUpdate(std::uint32_t const id, std::size_t const size)
  {
    ensureActive();
    return reserve(_cursorPtr.get(), utility::bytes::view(id), size, 0);
  }

  bool IntegerKeyDatabase::Writer::tryDelete(std::uint32_t const id)
  {
    ensureActive();
    return tryDeleteKey(_cursorPtr.get(), utility::bytes::view(id));
  }

  std::optional<std::span<std::byte const>> IntegerKeyDatabase::Writer::get(std::uint32_t const id) const
  {
    ensureActive();
    return readCursorPoint(_cursorPtr.get(), utility::bytes::view(id));
  }

  Result<> IntegerKeyDatabase::Writer::clear()
  {
    ensureActive();
    clearDatabase(_nativeTxn, _dbi);
    _lastId = 0;
    return {};
  }

  ByteKeyDatabase::Writer::Writer(DbiHandle const dbi, WriteTransaction& transaction)
    : _dbi{dbi}
    , _txn{&transaction}
    , _nativeTxn{detail::DatabaseAccess::handle(transaction)}
    , _bindingGeneration{detail::DatabaseAccess::bindingGeneration(transaction)}
  {
    ensureActive();
    _cursorPtr = Reader::create(detail::DatabaseAccess::handle(transaction), transaction, _dbi);
  }

  ByteKeyDatabase::Writer::Writer(Writer&& other) noexcept
    : _dbi{other._dbi}
    , _txn{std::exchange(other._txn, nullptr)}
    , _nativeTxn{std::exchange(other._nativeTxn, nullptr)}
    , _bindingGeneration{std::exchange(other._bindingGeneration, 0)}
    , _cursorPtr{std::move(other._cursorPtr)}
  {
  }

  ByteKeyDatabase::Writer& ByteKeyDatabase::Writer::operator=(Writer&& other) noexcept
  {
    if (this == &other)
    {
      return *this;
    }

    _dbi = other._dbi;
    _txn = std::exchange(other._txn, nullptr);
    _nativeTxn = std::exchange(other._nativeTxn, nullptr);
    _bindingGeneration = std::exchange(other._bindingGeneration, 0);
    _cursorPtr = std::move(other._cursorPtr);
    return *this;
  }

  ByteKeyDatabase::Writer::~Writer() noexcept = default;

  void ByteKeyDatabase::Writer::ensureActive() const
  {
    AO_EXPECTS(_txn != nullptr && _txn->isActive() &&
                 detail::DatabaseAccess::bindingGeneration(*_txn) == _bindingGeneration &&
                 detail::DatabaseAccess::handle(*_txn) == _nativeTxn,
               "ByteKeyDatabase::Writer used after its transaction finished or was replaced");
    AO_EXPECTS(detail::DatabaseAccess::isOnOwnerThread(*_txn),
               "ByteKeyDatabase::Writer used off its transaction's creating thread");
  }

  Result<> ByteKeyDatabase::Writer::create(std::span<std::byte const> const key, std::span<std::byte const> const data)
  {
    ensureActive();
    return putCopied(_cursorPtr.get(), key, data, MDB_NOOVERWRITE);
  }

  Result<> ByteKeyDatabase::Writer::update(std::span<std::byte const> const key, std::span<std::byte const> const data)
  {
    ensureActive();
    return putCopied(_cursorPtr.get(), key, data, 0);
  }

  bool ByteKeyDatabase::Writer::tryDelete(std::span<std::byte const> const key)
  {
    ensureActive();
    return tryDeleteKey(_cursorPtr.get(), key);
  }

  std::optional<std::span<std::byte const>> ByteKeyDatabase::Writer::get(std::span<std::byte const> const key) const
  {
    ensureActive();
    return readCursorPoint(_cursorPtr.get(), key);
  }

  Result<> ByteKeyDatabase::Writer::clear()
  {
    ensureActive();
    clearDatabase(_nativeTxn, _dbi);
    return {};
  }

  detail::UnvalidatedDatabase::UnvalidatedDatabase(UnvalidatedDatabase&& other) noexcept
    : _dbi{std::exchange(other._dbi, kInvalidDbi)}, _nativeFlags{std::exchange(other._nativeFlags, 0)}
  {
  }

  detail::UnvalidatedDatabase& detail::UnvalidatedDatabase::operator=(UnvalidatedDatabase&& other) noexcept
  {
    if (this == &other)
    {
      return *this;
    }

    _dbi = std::exchange(other._dbi, kInvalidDbi);
    _nativeFlags = std::exchange(other._nativeFlags, 0);
    return *this;
  }

  Result<detail::UnvalidatedDatabase> detail::UnvalidatedDatabase::openExisting(WriteTransaction& transaction,
                                                                                std::string const& name)
  {
    auto openedRes = openNamedDatabase(transaction, name, 0);

    if (!openedRes)
    {
      return std::unexpected{std::move(openedRes.error())};
    }

    return UnvalidatedDatabase{openedRes->dbi, openedRes->nativeFlags};
  }

  std::optional<std::span<std::byte const>> detail::UnvalidatedDatabase::getRaw(
    ReadTransaction const& transaction,
    std::span<std::byte const> const key) const
  {
    AO_EXPECTS(_dbi != kInvalidDbi, "UnvalidatedDatabase used after classification or move");
    AO_EXPECTS(transaction.isActive(), "UnvalidatedDatabase used with an inactive transaction");
    AO_EXPECTS(DatabaseAccess::isOnOwnerThread(transaction),
               "UnvalidatedDatabase used off its write transaction's creating thread");
    return readPoint(DatabaseAccess::handle(transaction), _dbi, key, DatabaseAccess::isTransactionOwned(transaction));
  }

  Result<IntegerKeyDatabase> detail::UnvalidatedDatabase::intoIntegerKey(std::string_view const databaseName) &&
  {
    AO_EXPECTS(_dbi != kInvalidDbi, "UnvalidatedDatabase classified after move");

    if (auto validationRes = validateExactFlags(databaseName, _nativeFlags, static_cast<std::uint32_t>(MDB_INTEGERKEY));
        !validationRes)
    {
      return std::unexpected{std::move(validationRes.error())};
    }

    _nativeFlags = 0;
    return DatabaseAccess::integerKeyDatabase(std::exchange(_dbi, kInvalidDbi));
  }
} // namespace ao::lmdb
