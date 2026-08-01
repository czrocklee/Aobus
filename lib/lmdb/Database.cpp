// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/lmdb/Database.h>

#include "detail/ResultError.h"
#include "detail/ThrowError.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/ExceptionFormat.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <gsl-lite/gsl-lite.hpp>
#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>

namespace ao::lmdb
{
  namespace
  {
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
    T read(::MDB_val val)
    {
      if (val.mv_size != sizeof(T))
      {
        throwException<Exception>("read: bad value size");
      }

      T value;
      std::memcpy(&value, val.mv_data, sizeof(T));
      return value;
    }
  } // namespace

  Database::Database(DbiHandle dbi, KeyKind kind)
    : _dbi{dbi}, _kind{kind}
  {
  }

  Result<Database> Database::open(WriteTransaction& txn, std::string const& name, KeyKind kind)
  {
    if (!txn.isActive())
    {
      return makeError(Error::Code::InvalidState, "Cannot open a database with a finished write transaction");
    }

    DbiHandle dbi = {};
    unsigned int flags = MDB_CREATE;

    if (kind == KeyKind::Integer)
    {
      flags |= MDB_INTEGERKEY;
    }

    int const code = ::mdb_dbi_open(txn._txnPtr.get(), name.c_str(), flags, &dbi);

    if (code != MDB_SUCCESS)
    {
      throwOnMutationError("mdb_dbi_open", code);
    }

    return Database{dbi, kind};
  }

  Result<Database> Database::open(ReadTransaction& txn, std::string const& name, KeyKind kind)
  {
    if (!txn.isActive())
    {
      return makeError(Error::Code::InvalidState, "Cannot open a database with an inactive read transaction");
    }

    DbiHandle dbi = {};
    unsigned int flags = 0;

    if (kind == KeyKind::Integer)
    {
      flags |= MDB_INTEGERKEY;
    }

    if (auto result = resultFromCode("mdb_dbi_open", ::mdb_dbi_open(txn._txnPtr.get(), name.c_str(), flags, &dbi));
        !result)
    {
      return std::unexpected{result.error()};
    }

    return Database{dbi, kind};
  }

  Database::Reader Database::reader(ReadTransaction const& txn) const
  {
    if (!txn.isActive())
    {
      throwException<Exception>("Database::Reader created from an inactive transaction");
    }

    return Reader{_dbi, txn._txnPtr.get(), txn, _kind};
  }

  Database::Writer Database::writer(WriteTransaction& txn) const
  {
    return Writer{_dbi, txn, _kind};
  }

  Database::Reader::Reader(::MDB_dbi dbi, ::MDB_txn* txn, ReadTransaction const& owner, KeyKind kind)
    : _dbi{dbi}, _txn{txn}, _owner{&owner}, _kind{kind}
  {
  }

  Database::Reader::Iterator Database::Reader::begin() const
  {
    ensureActive();
    return Iterator{_txn, *_owner, _dbi, false};
  }

  std::optional<std::span<std::byte const>> Database::Reader::get(std::uint32_t id) const
  {
    return get(utility::bytes::view(id));
  }

  std::optional<std::span<std::byte const>> Database::Reader::get(std::span<std::byte const> keyView) const
  {
    ensureActive();
    auto key = makeVal(keyView.data(), keyView.size());
    auto val = ::MDB_val{.mv_size = 0, .mv_data = nullptr};
    int const rc = ::mdb_get(_txn, _dbi, &key, &val);

    if (rc == MDB_NOTFOUND)
    {
      return std::nullopt;
    }

    throwOnError("mdb_get", rc);
    return utility::bytes::view(static_cast<void const*>(val.mv_data), val.mv_size);
  }

  std::size_t Database::Reader::entryCount() const
  {
    ensureActive();
    auto stat = ::MDB_stat{};
    throwOnError("mdb_stat", ::mdb_stat(_txn, _dbi, &stat));
    return stat.ms_entries;
  }

  std::uint32_t Database::Reader::maxKey() const
  {
    ensureActive();
    auto cursorPtr = create(_txn, _dbi);
    auto key = ::MDB_val{.mv_size = 0, .mv_data = nullptr};
    auto val = ::MDB_val{.mv_size = 0, .mv_data = nullptr};

    int const rc = ::mdb_cursor_get(cursorPtr.get(), &key, &val, MDB_LAST);

    if (rc == MDB_NOTFOUND)
    {
      return 0;
    }

    throwOnError("mdb_cursor_get", rc);
    return read<std::uint32_t>(key);
  }

  void Database::Reader::MdbCursorDeleter::operator()(MDB_cursor* cur) const noexcept
  {
    ::mdb_cursor_close(cur);
  }

  Database::Reader::CursorPtr Database::Reader::create(::MDB_txn* txn, ::MDB_dbi dbi)
  {
    ::MDB_cursor* cursor = nullptr;
    throwOnError("mdb_cursor_open", ::mdb_cursor_open(txn, dbi, &cursor));
    return CursorPtr{cursor};
  }

  void Database::Reader::ensureActive() const
  {
    if (_owner == nullptr || !_owner->isActive())
    {
      throwException<Exception>("Database::Reader used after its transaction finished");
    }
  }

  Database::Reader::KeyView::operator std::uint32_t() const
  {
    if (size() != sizeof(std::uint32_t))
    {
      throwException<Exception>("KeyView: cannot coerce {}-byte key to uint32", size());
    }

    std::uint32_t val = 0;
    std::memcpy(&val, data(), sizeof(val));
    return val;
  }

  Database::Reader::Iterator::Iterator(::MDB_txn* txn, ReadTransaction const& owner, ::MDB_dbi dbi, bool end)
    : _cursorPtr{Reader::create(txn, dbi)}, _owner{&owner}
  {
    if (end)
    {
      _cursorPtr.reset();
      return;
    }

    auto key = ::MDB_val{.mv_size = 0, .mv_data = nullptr};
    auto val = ::MDB_val{.mv_size = 0, .mv_data = nullptr};

    if (int const rc = ::mdb_cursor_get(_cursorPtr.get(), &key, &val, MDB_FIRST); rc == MDB_NOTFOUND)
    {
      _cursorPtr.reset();
    }
    else
    {
      throwOnError("mdb_cursor_get", rc);
      _value.first = Reader::KeyView{static_cast<std::byte const*>(key.mv_data), key.mv_size};
      _value.second = utility::bytes::view(static_cast<void const*>(val.mv_data), val.mv_size);
    }
  }

  Database::Reader::Iterator::Iterator(Iterator&& other) noexcept
    : _cursorPtr{std::move(other._cursorPtr)}, _value{other._value}, _owner{std::exchange(other._owner, nullptr)}
  {
    other._value = Reader::Value{};
  }

  Database::Reader::Iterator& Database::Reader::Iterator::operator=(Iterator&& other) noexcept
  {
    if (this == &other)
    {
      return *this;
    }

    releaseFinishedCursor();
    _cursorPtr = std::move(other._cursorPtr);
    _value = other._value;
    _owner = std::exchange(other._owner, nullptr);
    other._value = Reader::Value{};
    return *this;
  }

  Database::Reader::Iterator::~Iterator() noexcept
  {
    releaseFinishedCursor();
  }

  Database::Reader::Iterator::reference Database::Reader::Iterator::operator*() const
  {
    ensureActive();
    gsl_Expects(_cursorPtr != nullptr);
    return _value;
  }

  Database::Reader::Iterator::pointer Database::Reader::Iterator::operator->() const
  {
    ensureActive();
    gsl_Expects(_cursorPtr != nullptr);
    return &_value;
  }

  Database::Reader::Iterator& Database::Reader::Iterator::operator++()
  {
    ensureActive();
    gsl_Expects(_cursorPtr != nullptr);
    next();
    return *this;
  }

  bool Database::Reader::Iterator::operator==(Iterator const& other) const
  {
    return _cursorPtr == other._cursorPtr;
  }

  void Database::Reader::Iterator::ensureActive() const
  {
    if (_owner == nullptr || !_owner->isActive())
    {
      throwException<Exception>("Database::Reader::Iterator used after its transaction finished");
    }
  }

  void Database::Reader::Iterator::releaseFinishedCursor() noexcept
  {
    // LMDB closes cursors when a write transaction commits or aborts. If this
    // iterator borrowed such a cursor, relinquish the stale handle.
    if (_owner != nullptr && !_owner->isActive())
    {
      std::ignore = _cursorPtr.release();
    }
  }

  void Database::Reader::Iterator::next()
  {
    auto key = ::MDB_val{.mv_size = 0, .mv_data = nullptr};
    auto val = ::MDB_val{.mv_size = 0, .mv_data = nullptr};

    if (int const rc = ::mdb_cursor_get(_cursorPtr.get(), &key, &val, MDB_NEXT); rc == MDB_NOTFOUND)
    {
      _value = Reader::Value{};
      _cursorPtr.reset();
    }
    else
    {
      throwOnError("mdb_cursor_get", rc);
      _value.first = Reader::KeyView{static_cast<std::byte const*>(key.mv_data), key.mv_size};
      _value.second = utility::bytes::view(static_cast<void const*>(val.mv_data), val.mv_size);
    }
  }

  Database::Writer::Writer(::MDB_dbi dbi, WriteTransaction& txn, Database::KeyKind kind)
    : _dbi{dbi}, _txn{&txn}, _kind{kind}
  {
    ensureActive();
    _cursorPtr = Reader::create(txn._txnPtr.get(), _dbi);

    if (_kind == Database::KeyKind::Integer)
    {
      auto key = ::MDB_val{.mv_size = 0, .mv_data = nullptr};

      int const rc = ::mdb_cursor_get(_cursorPtr.get(), &key, nullptr, MDB_LAST);

      if (rc == MDB_SUCCESS)
      {
        _lastId = read<std::uint32_t>(key);
      }
      else if (rc != MDB_NOTFOUND)
      {
        throwOnError("mdb_cursor_get", rc);
      }
    }
  }

  Database::Writer::Writer(Writer&& other) noexcept
    : _dbi{other._dbi}
    , _txn{std::exchange(other._txn, nullptr)}
    , _cursorPtr{std::move(other._cursorPtr)}
    , _lastId{other._lastId}
    , _kind{other._kind}
  {
  }

  Database::Writer& Database::Writer::operator=(Writer&& other) noexcept
  {
    if (this == &other)
    {
      return *this;
    }

    releaseFinishedCursor();
    _dbi = other._dbi;
    _txn = std::exchange(other._txn, nullptr);
    _cursorPtr = std::move(other._cursorPtr);
    _lastId = other._lastId;
    _kind = other._kind;
    return *this;
  }

  Database::Writer::~Writer() noexcept
  {
    releaseFinishedCursor();
  }

  void Database::Writer::releaseFinishedCursor() noexcept
  {
    // LMDB frees write-transaction cursors when that transaction commits or
    // aborts. Relinquish our stale pointer so the deleter cannot close it twice.
    if (_txn != nullptr && _txn->isFinished())
    {
      std::ignore = _cursorPtr.release();
    }
  }

  void Database::Writer::ensureActive() const
  {
    if (_txn == nullptr || !_txn->isActive())
    {
      throwException<Exception>("Database::Writer used after its transaction finished");
    }
  }

  namespace
  {
    std::int32_t put(::MDB_cursor* cursor,
                     std::span<std::byte const> keyView,
                     std::span<std::byte const> data,
                     unsigned int flags)
    {
      gsl_Expects(cursor != nullptr);

      auto key = makeVal(keyView.data(), keyView.size());
      auto val = makeVal(data.data(), data.size());
      return ::mdb_cursor_put(cursor, &key, &val, flags);
    }

    struct ReserveResult final
    {
      std::int32_t code = MDB_SUCCESS;
      ::MDB_val value{.mv_size = 0, .mv_data = nullptr};
    };

    ReserveResult reserve(::MDB_cursor* cursor,
                          std::span<std::byte const> keyView,
                          std::size_t size,
                          std::uint32_t flags)
    {
      gsl_Expects(cursor != nullptr);

      auto key = makeVal(keyView.data(), keyView.size());
      auto val = makeVal(nullptr, size);
      auto const code = ::mdb_cursor_put(cursor, &key, &val, flags | MDB_RESERVE);
      return {.code = code, .value = val};
    }
  } // namespace

  Result<> Database::Writer::create(std::uint32_t id, std::span<std::byte const> data)
  {
    return create(utility::bytes::view(id), data);
  }

  Result<> Database::Writer::create(std::span<std::byte const> key, std::span<std::byte const> data)
  {
    ensureActive();
    auto const code = put(_cursorPtr.get(), key, data, MDB_NOOVERWRITE);

    if (code != MDB_SUCCESS && code != MDB_KEYEXIST)
    {
      throwOnMutationError("mdb_cursor_put", code);
    }

    return resultFromCode("mdb_cursor_put", code);
  }

  Result<std::span<std::byte>> Database::Writer::create(std::uint32_t id, std::size_t size)
  {
    return create(utility::bytes::view(id), size);
  }

  Result<std::span<std::byte>> Database::Writer::create(std::span<std::byte const> key, std::size_t size)
  {
    ensureActive();
    auto const result = reserve(_cursorPtr.get(), key, size, MDB_NOOVERWRITE);

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

  Result<std::uint32_t> Database::Writer::append(std::span<std::byte const> data)
  {
    ensureActive();

    if (_lastId == std::numeric_limits<std::uint32_t>::max())
    {
      return makeError(Error::Code::ResourceExhausted, "LMDB integer key space exhausted");
    }

    auto id = ++_lastId;

    if (auto result = create(id, data); !result)
    {
      --_lastId;
      return std::unexpected{result.error()};
    }

    return id;
  }

  Result<std::pair<std::uint32_t, std::span<std::byte>>> Database::Writer::append(std::size_t size)
  {
    ensureActive();

    if (_lastId == std::numeric_limits<std::uint32_t>::max())
    {
      return makeError(Error::Code::ResourceExhausted, "LMDB integer key space exhausted");
    }

    auto id = ++_lastId;
    auto data = create(id, size);

    if (!data)
    {
      --_lastId;
      return std::unexpected{data.error()};
    }

    return std::pair{id, *data};
  }

  Result<> Database::Writer::update(std::uint32_t id, std::span<std::byte const> data)
  {
    return update(utility::bytes::view(id), data);
  }

  Result<> Database::Writer::update(std::span<std::byte const> key, std::span<std::byte const> data)
  {
    ensureActive();
    auto const code = put(_cursorPtr.get(), key, data, 0);

    if (code != MDB_SUCCESS)
    {
      throwOnMutationError("mdb_cursor_put", code);
    }

    return resultFromCode("mdb_cursor_put", code);
  }

  Result<std::span<std::byte>> Database::Writer::update(std::uint32_t id, std::size_t size)
  {
    return update(utility::bytes::view(id), size);
  }

  Result<std::span<std::byte>> Database::Writer::update(std::span<std::byte const> key, std::size_t size)
  {
    ensureActive();
    auto const result = reserve(_cursorPtr.get(), key, size, 0);

    if (result.code == MDB_SUCCESS)
    {
      return utility::bytes::view(result.value.mv_data, result.value.mv_size);
    }

    throwOnMutationError("mdb_cursor_put", result.code);
  }

  bool Database::Writer::del(std::uint32_t id)
  {
    return del(utility::bytes::view(id));
  }

  bool Database::Writer::del(std::span<std::byte const> keyView)
  {
    ensureActive();
    auto key = makeVal(keyView.data(), keyView.size());
    int const rc = ::mdb_cursor_get(_cursorPtr.get(), &key, nullptr, MDB_SET);

    if (rc == MDB_NOTFOUND)
    {
      return false;
    }

    if (rc != MDB_SUCCESS)
    {
      throwOnMutationError("mdb_cursor_get", rc);
    }

    if (int const deleteCode = ::mdb_cursor_del(_cursorPtr.get(), 0); deleteCode != MDB_SUCCESS)
    {
      throwOnMutationError("mdb_cursor_del", deleteCode);
    }

    return true;
  }

  std::optional<std::span<std::byte const>> Database::Writer::get(std::uint32_t id) const
  {
    return get(utility::bytes::view(id));
  }

  std::optional<std::span<std::byte const>> Database::Writer::get(std::span<std::byte const> keyView) const
  {
    ensureActive();
    auto key = makeVal(keyView.data(), keyView.size());
    auto val = ::MDB_val{.mv_size = 0, .mv_data = nullptr};
    int const rc = ::mdb_cursor_get(_cursorPtr.get(), &key, &val, MDB_SET);

    if (rc == MDB_NOTFOUND)
    {
      return std::nullopt;
    }

    throwOnError("mdb_cursor_get", rc);
    return utility::bytes::view(static_cast<void const*>(val.mv_data), val.mv_size);
  }

  Result<> Database::Writer::clear()
  {
    ensureActive();
    int const code = ::mdb_drop(_txn->_txnPtr.get(), _dbi, 0);

    if (code != MDB_SUCCESS)
    {
      throwOnMutationError("mdb_drop", code);
    }

    return resultFromCode("mdb_drop", code);
  }
} // namespace ao::lmdb
