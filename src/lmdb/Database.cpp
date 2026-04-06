// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ThrowError.h"
#include <rs/Exception.h>
#include <rs/lmdb/Database.h>
#include <rs/utility/ByteView.h>

#include <cstring>
#include <lmdb.h>
#include <vector>

namespace rs::lmdb
{
  namespace
  {
    template<typename T>
    MDB_val makeVal(T const& val)
    {
      return {.mv_size = sizeof(T), .mv_data = const_cast<T*>(&val)};
    }

    inline MDB_val makeVal(void const* data = nullptr, std::size_t size = 0)
    {
      return {.mv_size = size, .mv_data = const_cast<void*>(data)}; // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }

    std::span<std::byte> asBytes(MDB_val const& val)
    {
      return {static_cast<std::byte*>(val.mv_data), val.mv_size};
    }

    template<typename T>
    T read(MDB_val val)
    {
      if (val.mv_size != sizeof(T))
      {
        RS_THROW(rs::Exception, "read: bad value size");
      }

      T value;
      std::memcpy(&value, val.mv_data, sizeof(T));
      return value;
    }
  }

  using Writer = Database::Writer;
  using Reader = Database::Reader;

  Database::Database(WriteTransaction& txn, std::string const& db)
  {
    throwOnError("mdb_dbi_open", mdb_dbi_open(txn._handle.get(), db.c_str(), MDB_CREATE | MDB_INTEGERKEY, &_dbi));
  }

  Database::Database(ReadTransaction& txn, std::string const& db)
  {
    throwOnError("mdb_dbi_open", mdb_dbi_open(txn._handle.get(), db.c_str(), MDB_INTEGERKEY, &_dbi));
  }

  Reader Database::reader(ReadTransaction& txn) const
  {
    return Reader{_dbi, txn._handle.get()};
  }

  Writer Database::writer(WriteTransaction& txn) const
  {
    return Writer{_dbi, txn};
  }

  Reader::Reader(MDB_dbi dbi, MDB_txn* txn)
    : _dbi{dbi}, _txn{txn}
  {
  }

  std::optional<std::span<std::byte const>> Reader::get(std::uint32_t id) const
  {
    auto key = makeVal(id);
    auto value = makeVal(nullptr, 0);
    int const rc = mdb_get(_txn, _dbi, &key, &value);

    if (rc == MDB_NOTFOUND)
    {
      return std::nullopt;
    }

    throwOnError("mdb_get", rc);
    return utility::asBytes(static_cast<std::byte const*>(value.mv_data), value.mv_size);
  }

  std::uint32_t Reader::maxKey() const
  {
    auto cursor = create(_txn, _dbi);
    auto key = MDB_val{0, nullptr};
    auto val = makeVal(nullptr, 0);
    int const rc = mdb_cursor_get(cursor.get(), &key, &val, MDB_LAST);

    if (rc == MDB_NOTFOUND)
    {
      return 0;
    }

    throwOnError("mdb_cursor_get", rc);
    return read<std::uint32_t>(key);
  }

  Reader::CursorPtr Reader::create(MDB_txn* txn, MDB_dbi dbi)
  {
    MDB_cursor* cursor = nullptr;
    throwOnError("mdb_cursor_open", mdb_cursor_open(txn, dbi, &cursor));
    return CursorPtr{cursor};
  }

  Reader::Iterator Reader::begin() const
  {
    return Iterator{Reader::create(_txn, _dbi)};
  }

  Reader::Iterator Reader::end() const
  {
    return Iterator{};
  }

  Reader::Iterator::Iterator() = default;

  Reader::Iterator::Iterator(CursorPtr cursor)
    : _cursor{std::move(cursor)}
  {
    increment();
  }

  Reader::Iterator::~Iterator() = default;

  Reader::Iterator::Iterator(Iterator const& other)
    : _value{other._value}
  {
    if (other._cursor)
    {
      _cursor = Reader::create(mdb_cursor_txn(other._cursor.get()), mdb_cursor_dbi(other._cursor.get()));
      auto key = makeVal(&_value.first, sizeof(std::uint32_t));
      throwOnError("mdb_cursor_get", mdb_cursor_get(_cursor.get(), &key, nullptr, MDB_SET));
    }
  }

  Reader::Iterator::Iterator(Iterator&& other) noexcept
    : _cursor{std::move(other._cursor)}, _value{other._value}
  {
  }

  bool Reader::Iterator::equal(Iterator const& other) const
  {
    return (_cursor != nullptr) == (other._cursor != nullptr) && _value.first == other._value.first;
  }

  void Reader::Iterator::increment()
  {
    auto key = makeVal();
    auto val = makeVal();

    if (int const rc = mdb_cursor_get(_cursor.get(), &key, &val, MDB_NEXT); rc == MDB_NOTFOUND)
    {
      _value = Value{};
      _cursor.reset();
    }
    else
    {
      throwOnError("mdb_cursor_get", rc);
      _value.first = read<std::uint32_t>(key);
      _value.second = asBytes(val);
    }
  }

  Reader::Value const& Reader::Iterator::dereference() const
  {
    return _value;
  }

  Writer::Writer(MDB_dbi dbi, WriteTransaction& txn)
    : _dbi{dbi}, _txn{&txn}, _cursor{Reader::create(txn._handle.get(), _dbi)}
  {
    auto key = MDB_val{0, nullptr};

    if (int const rc = mdb_cursor_get(_cursor.get(), &key, nullptr, MDB_LAST); rc == MDB_SUCCESS)
    {
      _lastId = read<std::uint32_t>(key);
    }
    else if (rc != MDB_NOTFOUND)
    {
      throwOnError("mdb_cursor_get", rc);
    }
  }

  Writer::~Writer()
  {
    // When transaction is committed, LMDB automatically closes all cursors - release without closing
    if (_txn->isCommitted())
    {
      std::ignore = _cursor.release();
    }
  }

  namespace
  {
    void put(MDB_cursor* cursor, std::uint32_t id, std::span<std::byte const> data, unsigned int flags)
    {
      auto key = makeVal(id);
      auto val = makeVal(data.data(), data.size());
      int const rc = mdb_cursor_put(cursor, &key, &val, flags);
      throwOnError("mdb_cursor_put", rc);
    }

    std::span<std::byte> reserve(MDB_cursor* cursor, std::uint32_t id, std::size_t size, unsigned int flags)
    {
      auto key = makeVal(id);
      auto val = makeVal(nullptr, size);
      throwOnError("mdb_cursor_put", mdb_cursor_put(cursor, &key, &val, flags | MDB_RESERVE));
      return asBytes(val);
    }
  }

  void Writer::create(std::uint32_t id, std::span<std::byte const> data)
  {
    put(_cursor.get(), id, data, MDB_NOOVERWRITE);
  }

  std::span<std::byte> Writer::create(std::uint32_t id, std::size_t size)
  {
    return reserve(_cursor.get(), id, size, MDB_NOOVERWRITE);
  }

  std::uint32_t Writer::append(std::span<std::byte const> data)
  {
    auto id = ++_lastId;
    put(_cursor.get(), id, data, MDB_NOOVERWRITE | MDB_APPEND);
    return id;
  }

  std::pair<std::uint32_t, std::span<std::byte>> Writer::append(std::size_t size)
  {
    auto id = ++_lastId;
    auto data = reserve(_cursor.get(), id, size, MDB_NOOVERWRITE | MDB_APPEND);
    return {id, data};
  }

  void Writer::update(std::uint32_t id, std::span<std::byte const> data)
  {
    put(_cursor.get(), id, data, 0);
  }

  std::span<std::byte> Writer::update(std::uint32_t id, std::size_t size)
  {
    return reserve(_cursor.get(), id, size, 0);
  }

  bool Writer::del(std::uint32_t id)
  {
    auto key = makeVal(&id, sizeof(id));
    int const rc = mdb_del(_txn->_handle.get(), _dbi, &key, nullptr);

    if (rc == MDB_NOTFOUND)
    {
      return false;
    }

    throwOnError("mdb_del", rc);
    return true;
  }

  std::optional<std::span<std::byte const>> Writer::get(std::uint32_t id) const
  {
    auto key = makeVal(id);
    auto val = makeVal();
    int const rc = mdb_get(_txn->_handle.get(), _dbi, &key, &val);

    if (rc == MDB_NOTFOUND)
    {
      return std::nullopt;
    }

    throwOnError("mdb_get", rc);
    return asBytes(val);
  }
}
