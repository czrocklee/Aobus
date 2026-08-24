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

      static bool transactionOwned(ReadTransaction const& transaction) noexcept
      {
        return transaction._failureMode == ReadTransaction::ReadFailureMode::Transaction;
      }

      static void acquireOpenAdmission(WriteTransaction& transaction) { transaction.acquireDatabaseOpenAdmission(); }

      static IntegerKeyDatabase integerKeyDatabase(DbiHandle const dbi) noexcept { return IntegerKeyDatabase{dbi}; }

      static ByteKeyDatabase byteKeyDatabase(DbiHandle const dbi) noexcept { return ByteKeyDatabase{dbi}; }
    };
  } // namespace detail

  namespace
  {
    constexpr auto kInvalidDbi = std::numeric_limits<DbiHandle>::max();

    template<typename T>
    ::MDB_val makeVal(T const& val)
    {
      return {.mv_size = sizeof(T), .mv_data = utility::layout::asLegacyPtr<T>(&val)};
    }

    inline ::MDB_val makeVal(void const* data = nullptr, std::size_t size = 0)
    {
      return {.mv_size = size, .mv_data = utility::layout::asLegacyPtr<void>(data)};
    }

    template<typename T>
    T read(::MDB_val const val)
    {
      AO_INVARIANT(val.mv_size == sizeof(T), "LMDB integer key has an invalid size");

      T value;
      std::memcpy(&value, val.mv_data, sizeof(T));
      return value;
    }

    struct OpenedDatabase final
    {
      DbiHandle dbi = 0;
      std::uint32_t nativeFlags = 0;
    };

    Result<OpenedDatabase> openNamedDatabase(MDB_txn* transaction,
                                             std::string const& name,
                                             std::uint32_t const openFlags)
    {
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

      unsigned int nativeFlags = 0;
      auto const flagsCode = ::mdb_dbi_flags(transaction, dbi, &nativeFlags);

      if (flagsCode != MDB_SUCCESS)
      {
        throwOnMutationError("mdb_dbi_flags", flagsCode);
      }

      return OpenedDatabase{.dbi = dbi, .nativeFlags = nativeFlags};
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
      auto key = makeVal(keyView.data(), keyView.size());
      auto value = ::MDB_val{.mv_size = 0, .mv_data = nullptr};
      auto const code = ::mdb_get(transaction, dbi, &key, &value);

      if (code == MDB_NOTFOUND)
      {
        return std::nullopt;
      }

      failRead("mdb_get", code, transactionOwned);
      return utility::bytes::view(static_cast<void const*>(value.mv_data), value.mv_size);
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

    bool positionCursor(MDB_cursor* cursor,
                        MDB_cursor_op const operation,
                        bool const transactionOwned,
                        RawRecord& record)
    {
      auto key = ::MDB_val{.mv_size = 0, .mv_data = nullptr};
      auto value = ::MDB_val{.mv_size = 0, .mv_data = nullptr};
      auto const code = ::mdb_cursor_get(cursor, &key, &value, operation);

      if (code == MDB_NOTFOUND)
      {
        record = {};
        return false;
      }

      failRead("mdb_cursor_get", code, transactionOwned);
      record = {.key = utility::bytes::view(static_cast<void const*>(key.mv_data), key.mv_size),
                .value = utility::bytes::view(static_cast<void const*>(value.mv_data), value.mv_size)};
      return true;
    }

    bool positionCursorAtOrAfter(MDB_cursor* cursor,
                                 std::span<std::byte const> const keyView,
                                 bool const transactionOwned,
                                 RawRecord& record)
    {
      auto key = makeVal(keyView.data(), keyView.size());
      auto value = ::MDB_val{.mv_size = 0, .mv_data = nullptr};
      auto const code = ::mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);

      if (code == MDB_NOTFOUND)
      {
        record = {};
        return false;
      }

      failRead("mdb_cursor_get", code, transactionOwned);
      record = {.key = utility::bytes::view(static_cast<void const*>(key.mv_data), key.mv_size),
                .value = utility::bytes::view(static_cast<void const*>(value.mv_data), value.mv_size)};
      return true;
    }

    std::int32_t put(MDB_cursor* cursor,
                     std::span<std::byte const> const keyView,
                     std::span<std::byte const> const data,
                     unsigned int const flags)
    {
      AO_INVARIANT(cursor != nullptr);

      auto key = makeVal(keyView.data(), keyView.size());
      auto value = makeVal(data.data(), data.size());
      return ::mdb_cursor_put(cursor, &key, &value, flags);
    }

    struct ReserveResult final
    {
      std::int32_t code = MDB_SUCCESS;
      ::MDB_val value{.mv_size = 0, .mv_data = nullptr};
    };

    ReserveResult reserve(MDB_cursor* cursor,
                          std::span<std::byte const> const keyView,
                          std::size_t const size,
                          std::uint32_t const flags)
    {
      AO_INVARIANT(cursor != nullptr);

      auto key = makeVal(keyView.data(), keyView.size());
      auto value = makeVal(nullptr, size);
      auto const code = ::mdb_cursor_put(cursor, &key, &value, flags | MDB_RESERVE);
      return {.code = code, .value = value};
    }

    bool deleteKey(MDB_cursor* cursor, std::span<std::byte const> const keyView)
    {
      auto key = makeVal(keyView.data(), keyView.size());
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
      auto key = makeVal(keyView.data(), keyView.size());
      auto value = ::MDB_val{.mv_size = 0, .mv_data = nullptr};
      auto const code = ::mdb_cursor_get(cursor, &key, &value, MDB_SET);

      if (code == MDB_NOTFOUND)
      {
        return std::nullopt;
      }

      failRead("mdb_cursor_get", code, true);
      return utility::bytes::view(static_cast<void const*>(value.mv_data), value.mv_size);
    }

    Result<> clearDatabase(MDB_txn* transaction, DbiHandle const dbi)
    {
      auto const code = ::mdb_drop(transaction, dbi, 0);

      if (code != MDB_SUCCESS)
      {
        throwOnMutationError("mdb_drop", code);
      }

      return resultFromCode("mdb_drop", code);
    }
  } // namespace

  Result<IntegerKeyDatabase> IntegerKeyDatabase::open(WriteTransaction& transaction, std::string const& name)
  {
    AO_EXPECTS(transaction.isActive(), "Cannot open a database with a finished write transaction");
    detail::DatabaseAccess::acquireOpenAdmission(transaction);
    auto openedRes = openNamedDatabase(
      detail::DatabaseAccess::handle(transaction), name, static_cast<std::uint32_t>(MDB_CREATE | MDB_INTEGERKEY));

    if (!openedRes)
    {
      return std::unexpected{std::move(openedRes.error())};
    }

    if (auto validationRes =
          validateExactFlags(name, openedRes->nativeFlags, static_cast<std::uint32_t>(MDB_INTEGERKEY));
        !validationRes)
    {
      return std::unexpected{std::move(validationRes.error())};
    }

    return detail::DatabaseAccess::integerKeyDatabase(openedRes->dbi);
  }

  Result<IntegerKeyDatabase> IntegerKeyDatabase::openExisting(WriteTransaction& transaction, std::string const& name)
  {
    AO_EXPECTS(transaction.isActive(), "Cannot open a database with a finished write transaction");
    detail::DatabaseAccess::acquireOpenAdmission(transaction);
    auto openedRes = openNamedDatabase(detail::DatabaseAccess::handle(transaction), name, 0);

    if (!openedRes)
    {
      return std::unexpected{std::move(openedRes.error())};
    }

    if (auto validationRes =
          validateExactFlags(name, openedRes->nativeFlags, static_cast<std::uint32_t>(MDB_INTEGERKEY));
        !validationRes)
    {
      return std::unexpected{std::move(validationRes.error())};
    }

    return detail::DatabaseAccess::integerKeyDatabase(openedRes->dbi);
  }

  Result<ByteKeyDatabase> ByteKeyDatabase::open(WriteTransaction& transaction, std::string const& name)
  {
    AO_EXPECTS(transaction.isActive(), "Cannot open a database with a finished write transaction");
    detail::DatabaseAccess::acquireOpenAdmission(transaction);
    auto openedRes =
      openNamedDatabase(detail::DatabaseAccess::handle(transaction), name, static_cast<std::uint32_t>(MDB_CREATE));

    if (!openedRes)
    {
      return std::unexpected{std::move(openedRes.error())};
    }

    if (auto validationRes = validateExactFlags(name, openedRes->nativeFlags, 0); !validationRes)
    {
      return std::unexpected{std::move(validationRes.error())};
    }

    return detail::DatabaseAccess::byteKeyDatabase(openedRes->dbi);
  }

  Result<ByteKeyDatabase> ByteKeyDatabase::openExisting(WriteTransaction& transaction, std::string const& name)
  {
    AO_EXPECTS(transaction.isActive(), "Cannot open a database with a finished write transaction");
    detail::DatabaseAccess::acquireOpenAdmission(transaction);
    auto openedRes = openNamedDatabase(detail::DatabaseAccess::handle(transaction), name, 0);

    if (!openedRes)
    {
      return std::unexpected{std::move(openedRes.error())};
    }

    if (auto validationRes = validateExactFlags(name, openedRes->nativeFlags, 0); !validationRes)
    {
      return std::unexpected{std::move(validationRes.error())};
    }

    return detail::DatabaseAccess::byteKeyDatabase(openedRes->dbi);
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

    unsigned int nativeFlags = 0;
    auto const flagsCode = ::mdb_dbi_flags(detail::DatabaseAccess::handle(transaction), dbi, &nativeFlags);

    if (flagsCode != MDB_SUCCESS)
    {
      throwOnMutationError("mdb_dbi_flags", flagsCode);
    }

    if (auto validationRes = validateByteKeyMain(nativeFlags); !validationRes)
    {
      return std::unexpected{std::move(validationRes.error())};
    }

    return detail::DatabaseAccess::byteKeyDatabase(dbi);
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
    : _dbi{dbi}, _txn{transaction}, _owner{&owner}
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
    return readPoint(_txn, _dbi, utility::bytes::view(id), detail::DatabaseAccess::transactionOwned(*_owner));
  }

  std::size_t IntegerKeyDatabase::Reader::entryCount() const
  {
    ensureActive();
    return readEntryCount(_txn, _dbi, detail::DatabaseAccess::transactionOwned(*_owner));
  }

  std::uint32_t IntegerKeyDatabase::Reader::maxKey() const
  {
    ensureActive();
    auto cursorPtr = create(_txn, *_owner, _dbi);
    auto record = RawRecord{};

    if (!positionCursor(cursorPtr.get(), MDB_LAST, detail::DatabaseAccess::transactionOwned(*_owner), record))
    {
      return 0;
    }

    return read<std::uint32_t>(makeVal(record.key.data(), record.key.size()));
  }

  void IntegerKeyDatabase::Reader::MdbCursorDeleter::operator()(MDB_cursor* cursor) const noexcept
  {
    if (cleanup == detail::CursorCleanup::Explicit)
    {
      ::mdb_cursor_close(cursor);
    }
  }

  IntegerKeyDatabase::Reader::CursorPtr IntegerKeyDatabase::Reader::create(MDB_txn* transaction,
                                                                           ReadTransaction const& owner,
                                                                           DbiHandle const dbi)
  {
    auto const transactionOwned = detail::DatabaseAccess::transactionOwned(owner);
    auto const cleanup = transactionOwned ? detail::CursorCleanup::WriteTransaction : detail::CursorCleanup::Explicit;
    return CursorPtr{openCursor(transaction, dbi, transactionOwned), MdbCursorDeleter{.cleanup = cleanup}};
  }

  void IntegerKeyDatabase::Reader::ensureActive() const
  {
    AO_EXPECTS(
      _owner != nullptr && _owner->isActive(), "IntegerKeyDatabase::Reader used after its transaction finished");
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
    : _cursorPtr{Reader::create(transaction, owner, dbi)}, _owner{&owner}
  {
    auto record = RawRecord{};

    if (!positionCursor(_cursorPtr.get(), MDB_FIRST, detail::DatabaseAccess::transactionOwned(owner), record))
    {
      _cursorPtr.reset();
      return;
    }

    _value = Reader::Value{Reader::KeyView{record.key}, record.value};
  }

  IntegerKeyDatabase::Reader::Iterator::Iterator(Iterator&& other) noexcept
    : _cursorPtr{std::move(other._cursorPtr)}, _value{other._value}, _owner{std::exchange(other._owner, nullptr)}
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
    _owner = std::exchange(other._owner, nullptr);
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
    AO_EXPECTS(_owner != nullptr && _owner->isActive(),
               "IntegerKeyDatabase::Reader::Iterator used after its transaction finished");
  }

  void IntegerKeyDatabase::Reader::Iterator::next()
  {
    auto record = RawRecord{};

    if (!positionCursor(_cursorPtr.get(), MDB_NEXT, detail::DatabaseAccess::transactionOwned(*_owner), record))
    {
      _value = Reader::Value{Reader::KeyView{std::span<std::byte const>{}}, std::span<std::byte const>{}};
      _cursorPtr.reset();
      return;
    }

    _value = Reader::Value{Reader::KeyView{record.key}, record.value};
  }

  ByteKeyDatabase::Reader::Reader(DbiHandle const dbi, MDB_txn* transaction, ReadTransaction const& owner)
    : _dbi{dbi}, _txn{transaction}, _owner{&owner}
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
    return readPoint(_txn, _dbi, key, detail::DatabaseAccess::transactionOwned(*_owner));
  }

  std::size_t ByteKeyDatabase::Reader::entryCount() const
  {
    ensureActive();
    return readEntryCount(_txn, _dbi, detail::DatabaseAccess::transactionOwned(*_owner));
  }

  void ByteKeyDatabase::Reader::MdbCursorDeleter::operator()(MDB_cursor* cursor) const noexcept
  {
    if (cleanup == detail::CursorCleanup::Explicit)
    {
      ::mdb_cursor_close(cursor);
    }
  }

  ByteKeyDatabase::Reader::CursorPtr ByteKeyDatabase::Reader::create(MDB_txn* transaction,
                                                                     ReadTransaction const& owner,
                                                                     DbiHandle const dbi)
  {
    auto const transactionOwned = detail::DatabaseAccess::transactionOwned(owner);
    auto const cleanup = transactionOwned ? detail::CursorCleanup::WriteTransaction : detail::CursorCleanup::Explicit;
    return CursorPtr{openCursor(transaction, dbi, transactionOwned), MdbCursorDeleter{.cleanup = cleanup}};
  }

  void ByteKeyDatabase::Reader::ensureActive() const
  {
    AO_EXPECTS(_owner != nullptr && _owner->isActive(), "ByteKeyDatabase::Reader used after its transaction finished");
  }

  ByteKeyDatabase::Reader::Iterator::Iterator(MDB_txn* transaction, ReadTransaction const& owner, DbiHandle const dbi)
    : _cursorPtr{Reader::create(transaction, owner, dbi)}, _owner{&owner}
  {
    auto record = RawRecord{};

    if (!positionCursor(_cursorPtr.get(), MDB_FIRST, detail::DatabaseAccess::transactionOwned(owner), record))
    {
      _cursorPtr.reset();
      return;
    }

    _value = Reader::Value{record.key, record.value};
  }

  ByteKeyDatabase::Reader::Iterator::Iterator(MDB_txn* transaction,
                                              ReadTransaction const& owner,
                                              DbiHandle const dbi,
                                              std::span<std::byte const> const lowerBoundKey)
    : _cursorPtr{Reader::create(transaction, owner, dbi)}, _owner{&owner}
  {
    auto record = RawRecord{};

    if (!positionCursorAtOrAfter(
          _cursorPtr.get(), lowerBoundKey, detail::DatabaseAccess::transactionOwned(owner), record))
    {
      _cursorPtr.reset();
      return;
    }

    _value = Reader::Value{record.key, record.value};
  }

  ByteKeyDatabase::Reader::Iterator::Iterator(Iterator&& other) noexcept
    : _cursorPtr{std::move(other._cursorPtr)}, _value{other._value}, _owner{std::exchange(other._owner, nullptr)}
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
    _owner = std::exchange(other._owner, nullptr);
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
    AO_EXPECTS(
      _owner != nullptr && _owner->isActive(), "ByteKeyDatabase::Reader::Iterator used after its transaction finished");
  }

  void ByteKeyDatabase::Reader::Iterator::next()
  {
    auto record = RawRecord{};

    if (!positionCursor(_cursorPtr.get(), MDB_NEXT, detail::DatabaseAccess::transactionOwned(*_owner), record))
    {
      _value = {};
      _cursorPtr.reset();
      return;
    }

    _value = Reader::Value{record.key, record.value};
  }

  IntegerKeyDatabase::Writer::Writer(DbiHandle const dbi, WriteTransaction& transaction)
    : _dbi{dbi}, _txn{&transaction}
  {
    ensureActive();
    _cursorPtr = Reader::create(detail::DatabaseAccess::handle(transaction), transaction, _dbi);

    if (auto record = RawRecord{}; positionCursor(_cursorPtr.get(), MDB_LAST, true, record))
    {
      _lastId = read<std::uint32_t>(makeVal(record.key.data(), record.key.size()));
    }
  }

  IntegerKeyDatabase::Writer::Writer(Writer&& other) noexcept
    : _dbi{other._dbi}
    , _txn{std::exchange(other._txn, nullptr)}
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
    _cursorPtr = std::move(other._cursorPtr);
    _lastId = other._lastId;
    return *this;
  }

  IntegerKeyDatabase::Writer::~Writer() noexcept = default;

  void IntegerKeyDatabase::Writer::ensureActive() const
  {
    AO_EXPECTS(_txn != nullptr && _txn->isActive(), "IntegerKeyDatabase::Writer used after its transaction finished");
  }

  Result<> IntegerKeyDatabase::Writer::create(std::uint32_t const id, std::span<std::byte const> const data)
  {
    ensureActive();
    auto const code = put(_cursorPtr.get(), utility::bytes::view(id), data, MDB_NOOVERWRITE);

    if (code != MDB_SUCCESS && code != MDB_KEYEXIST)
    {
      throwOnMutationError("mdb_cursor_put", code);
    }

    return resultFromCode("mdb_cursor_put", code);
  }

  Result<std::span<std::byte>> IntegerKeyDatabase::Writer::reserveCreate(std::uint32_t const id, std::size_t const size)
  {
    ensureActive();
    auto const result = reserve(_cursorPtr.get(), utility::bytes::view(id), size, MDB_NOOVERWRITE);

    if (result.code == MDB_SUCCESS)
    {
      return utility::bytes::view(result.value.mv_data, result.value.mv_size);
    }

    if (result.code != MDB_KEYEXIST)
    {
      throwOnMutationError("mdb_cursor_put", result.code);
    }

    return std::unexpected{resultFromCode("mdb_cursor_put", result.code).error()};
  }

  Result<std::uint32_t> IntegerKeyDatabase::Writer::append(std::span<std::byte const> const data)
  {
    ensureActive();

    if (_lastId == std::numeric_limits<std::uint32_t>::max())
    {
      return makeError(Error::Code::ResourceExhausted, "LMDB integer key space exhausted");
    }

    auto const id = ++_lastId;

    if (auto result = create(id, data); !result)
    {
      --_lastId;
      return std::unexpected{std::move(result.error())};
    }

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

    auto const id = ++_lastId;
    auto dataRes = reserveCreate(id, size);

    if (!dataRes)
    {
      --_lastId;
      return std::unexpected{std::move(dataRes.error())};
    }

    return std::pair{id, *dataRes};
  }

  Result<> IntegerKeyDatabase::Writer::update(std::uint32_t const id, std::span<std::byte const> const data)
  {
    ensureActive();
    auto const code = put(_cursorPtr.get(), utility::bytes::view(id), data, 0);

    if (code != MDB_SUCCESS)
    {
      throwOnMutationError("mdb_cursor_put", code);
    }

    return resultFromCode("mdb_cursor_put", code);
  }

  Result<std::span<std::byte>> IntegerKeyDatabase::Writer::reserveUpdate(std::uint32_t const id, std::size_t const size)
  {
    ensureActive();
    auto const result = reserve(_cursorPtr.get(), utility::bytes::view(id), size, 0);

    if (result.code == MDB_SUCCESS)
    {
      return utility::bytes::view(result.value.mv_data, result.value.mv_size);
    }

    throwOnMutationError("mdb_cursor_put", result.code);
  }

  bool IntegerKeyDatabase::Writer::del(std::uint32_t const id)
  {
    ensureActive();
    return deleteKey(_cursorPtr.get(), utility::bytes::view(id));
  }

  std::optional<std::span<std::byte const>> IntegerKeyDatabase::Writer::get(std::uint32_t const id) const
  {
    ensureActive();
    return readCursorPoint(_cursorPtr.get(), utility::bytes::view(id));
  }

  Result<> IntegerKeyDatabase::Writer::clear()
  {
    ensureActive();
    auto result = clearDatabase(detail::DatabaseAccess::handle(*_txn), _dbi);
    _lastId = 0;
    return result;
  }

  ByteKeyDatabase::Writer::Writer(DbiHandle const dbi, WriteTransaction& transaction)
    : _dbi{dbi}, _txn{&transaction}
  {
    ensureActive();
    _cursorPtr = Reader::create(detail::DatabaseAccess::handle(transaction), transaction, _dbi);
  }

  ByteKeyDatabase::Writer::Writer(Writer&& other) noexcept
    : _dbi{other._dbi}, _txn{std::exchange(other._txn, nullptr)}, _cursorPtr{std::move(other._cursorPtr)}
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
    _cursorPtr = std::move(other._cursorPtr);
    return *this;
  }

  ByteKeyDatabase::Writer::~Writer() noexcept = default;

  void ByteKeyDatabase::Writer::ensureActive() const
  {
    AO_EXPECTS(_txn != nullptr && _txn->isActive(), "ByteKeyDatabase::Writer used after its transaction finished");
  }

  Result<> ByteKeyDatabase::Writer::create(std::span<std::byte const> const key, std::span<std::byte const> const data)
  {
    ensureActive();
    auto const code = put(_cursorPtr.get(), key, data, MDB_NOOVERWRITE);

    if (code != MDB_SUCCESS && code != MDB_KEYEXIST)
    {
      throwOnMutationError("mdb_cursor_put", code);
    }

    return resultFromCode("mdb_cursor_put", code);
  }

  Result<> ByteKeyDatabase::Writer::update(std::span<std::byte const> const key, std::span<std::byte const> const data)
  {
    ensureActive();
    auto const code = put(_cursorPtr.get(), key, data, 0);

    if (code != MDB_SUCCESS)
    {
      throwOnMutationError("mdb_cursor_put", code);
    }

    return resultFromCode("mdb_cursor_put", code);
  }

  bool ByteKeyDatabase::Writer::del(std::span<std::byte const> const key)
  {
    ensureActive();
    return deleteKey(_cursorPtr.get(), key);
  }

  std::optional<std::span<std::byte const>> ByteKeyDatabase::Writer::get(std::span<std::byte const> const key) const
  {
    ensureActive();
    return readCursorPoint(_cursorPtr.get(), key);
  }

  Result<> ByteKeyDatabase::Writer::clear()
  {
    ensureActive();
    return clearDatabase(detail::DatabaseAccess::handle(*_txn), _dbi);
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
    AO_EXPECTS(transaction.isActive(), "Cannot open a database with a finished write transaction");
    DatabaseAccess::acquireOpenAdmission(transaction);
    auto openedRes = openNamedDatabase(DatabaseAccess::handle(transaction), name, 0);

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
    return readPoint(DatabaseAccess::handle(transaction), _dbi, key, DatabaseAccess::transactionOwned(transaction));
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
