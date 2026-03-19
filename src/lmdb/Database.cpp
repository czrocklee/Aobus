// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ThrowError.h"
#include <rs/lmdb/Database.h>

#include <cstring>
#include <lmdb.h>
#include <rs/utility/ByteView.h>
#include <vector>

namespace rs::lmdb
{
  namespace
  {
    template<typename T>
    MDB_val makeVal(T const& value)
    {
      return {sizeof(T), const_cast<T*>(&value)};
    }

    inline MDB_val makeVal(void const* data, std::size_t size)
    {
      return {size, const_cast<void*>(data)};
    }

    template<typename T>
    T read(std::string_view bytes)
    {
      if (bytes.size() != sizeof(T)) { throw std::runtime_error{"read: bad value size"}; }

      T value;
      std::memcpy(&value, bytes.data(), sizeof(T));
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
    throwOnError("mdb_dbi_open", mdb_dbi_open(txn._handle.get(), db.c_str(), MDB_CREATE | MDB_INTEGERKEY, &_dbi));
  }

  Database::~Database() = default;

  Reader Database::reader(ReadTransaction& txn) const
  {
    return Reader{_dbi, txn._handle.get()};
  }

  Writer Database::writer(WriteTransaction& txn)
  {
    return Writer{_dbi, txn};
  }

  Reader::Reader(MDB_dbi dbi, MDB_txn* txn) : _dbi{dbi}, _txn{txn}
  {
  }

  Reader::Iterator Reader::begin() const
  {
    MDB_cursor* cursor = nullptr;
    throwOnError("mdb_cursor_open", mdb_cursor_open(_txn, _dbi, &cursor));
    return Iterator{cursor};
  }

  Reader::Iterator Reader::end() const
  {
    return Iterator{};
  }

  std::optional<std::span<std::byte const>> Reader::get(std::uint32_t id) const
  {
    auto key = makeVal(id);
    auto value = makeVal(nullptr, 0);
    int const rc = mdb_get(_txn, _dbi, &key, &value);
    if (rc == MDB_NOTFOUND) { return std::nullopt; }
    throwOnError("mdb_get", rc);
    return std::span<std::byte const>{static_cast<std::byte const*>(value.mv_data), value.mv_size};
  }

  Reader::Iterator::Iterator() : _value{}
  {
  }

  Reader::Iterator::Iterator(MDB_cursor* cursor) : _cursor{cursor}
  {
    increment();
  }

  Reader::Iterator::~Iterator() = default;

  Reader::Iterator::Iterator(Iterator const& other) : _value{other._value}
  {
    if (other._cursor)
    {
      MDB_cursor* cursor = nullptr;
      throwOnError("mdb_cursor_open",
                   mdb_cursor_open(mdb_cursor_txn(other._cursor.get()), mdb_cursor_dbi(other._cursor.get()), &cursor));
      _cursor.reset(cursor);
      MDB_val keyValue{sizeof(std::uint32_t), const_cast<std::uint32_t*>(&_value.first)};
      throwOnError("mdb_cursor_get", mdb_cursor_get(_cursor.get(), &keyValue, nullptr, MDB_SET));
    }
  }

  Reader::Iterator::Iterator(Iterator&& other) noexcept : _cursor{std::move(other._cursor)}, _value{other._value}
  {
  }

  bool Reader::Iterator::equal(Iterator const& other) const
  {
    return (_cursor != nullptr) == (other._cursor != nullptr) && _value.first == other._value.first;
  }

  void Reader::Iterator::increment()
  {
    MDB_val keyValue{0, nullptr};
    MDB_val valueBuffer{0, nullptr};

    if (int const rc = mdb_cursor_get(_cursor.get(), &keyValue, &valueBuffer, MDB_NEXT); rc == MDB_NOTFOUND)
    {
      _value = Value{};
      _cursor.reset();
    }
    else
    {
      throwOnError("mdb_cursor_get", rc);
      std::string_view key{static_cast<char const*>(keyValue.mv_data), keyValue.mv_size};
      std::string_view value{static_cast<char const*>(valueBuffer.mv_data), valueBuffer.mv_size};
      _value.first = read<std::uint32_t>(key);
      _value.second = utility::asBytes(value);
    }
  }

  Reader::Value const& Reader::Iterator::dereference() const
  {
    return _value;
  }

  Writer::Writer(MDB_dbi dbi, WriteTransaction& txn) : _dbi{dbi}, _txn{txn}
  {
    MDB_cursor* cursor = nullptr;
    throwOnError("mdb_cursor_open", mdb_cursor_open(txn._handle.get(), _dbi, &cursor));
    _cursor.reset(cursor);
    MDB_val keyValue{0, nullptr};

    if (int const rc = mdb_cursor_get(_cursor.get(), &keyValue, nullptr, MDB_LAST); rc == MDB_SUCCESS)
    {
      std::string_view key{static_cast<char const*>(keyValue.mv_data), keyValue.mv_size};
      _lastId = read<std::uint32_t>(key);
    }
    else if (rc != MDB_NOTFOUND) { throwOnError("mdb_cursor_get", rc); }
  }

  Writer::Writer(Writer&& other) noexcept
    : _dbi{other._dbi}
    , _txn{other._txn}
    , _cursor{std::move(other._cursor)}
    , _lastId{other._lastId}
  {
  }

  Writer::~Writer() = default;

  namespace
  {
    void put(MDB_cursor* cursor,
             std::uint32_t id,
             std::span<std::byte const> data,
             unsigned int flags,
             std::vector<std::byte>& out)
    {
      auto keyValue = makeVal(id);
      auto valueBuffer = makeVal(data.data(), data.size());

      int const rc = mdb_cursor_put(cursor, &keyValue, &valueBuffer, flags);

      if (rc == MDB_KEYEXIST)
      {
        out.clear();
        return;
      }

      throwOnError("mdb_cursor_put", rc);

      // Get the actual stored value
      if (mdb_cursor_get(cursor, &keyValue, &valueBuffer, MDB_GET_CURRENT) != MDB_SUCCESS)
      {
        out.clear();
        return;
      }

      out.assign(static_cast<std::byte const*>(valueBuffer.mv_data),
                 static_cast<std::byte const*>(valueBuffer.mv_data) + valueBuffer.mv_size);
    }

    void reserve(MDB_cursor* cursor,
                 std::uint32_t id,
                 std::size_t size,
                 unsigned int flags,
                 std::vector<std::byte>& out)
    {
      auto keyValue = makeVal(id);
      auto valueBuffer = makeVal(nullptr, size);
      throwOnError("mdb_cursor_put", mdb_cursor_put(cursor, &keyValue, &valueBuffer, flags | MDB_RESERVE));
      out.resize(size);
      std::memcpy(out.data(), valueBuffer.mv_data, size);
    }
  }

  std::span<std::byte const> Writer::create(std::uint32_t id, std::span<std::byte const> data)
  {
    put(_cursor.get(), id, data, MDB_NOOVERWRITE, _lastData);
    return _lastData;
  }

  std::span<std::byte> Writer::create(std::uint32_t id, std::size_t size)
  {
    reserve(_cursor.get(), id, size, MDB_NOOVERWRITE, _lastData);
    return _lastData;
  }

  std::pair<std::uint32_t, std::span<std::byte const>> Writer::append(std::span<std::byte const> data)
  {
    auto id = ++_lastId;
    put(_cursor.get(), id, data, MDB_NOOVERWRITE | MDB_APPEND, _lastData);
    return {id, _lastData};
  }

  std::pair<std::uint32_t, std::span<std::byte>> Writer::append(std::size_t size)
  {
    auto id = ++_lastId;
    reserve(_cursor.get(), id, size, MDB_NOOVERWRITE | MDB_APPEND, _lastData);
    return {id, _lastData};
  }

  std::span<std::byte const> Writer::update(std::uint32_t id, std::span<std::byte const> data)
  {
    put(_cursor.get(), id, data, 0, _lastData);
    return _lastData;
  }

  bool Writer::del(std::uint32_t id)
  {
    MDB_val key{sizeof(id), const_cast<std::uint32_t*>(&id)};
    int const rc = mdb_del(_txn._handle.get(), _dbi, &key, nullptr);

    if (rc == MDB_NOTFOUND) { return false; }

    throwOnError("mdb_del", rc);
    return true;
  }

  std::optional<std::span<std::byte const>> Writer::get(std::uint32_t id) const
  {
    auto key = makeVal(id);
    auto value = makeVal(nullptr, 0);
    int const rc = mdb_get(_txn._handle.get(), _dbi, &key, &value);
    if (rc == MDB_NOTFOUND) { return std::nullopt; }
    throwOnError("mdb_get", rc);
    return std::span<std::byte const>{static_cast<std::byte const*>(value.mv_data), value.mv_size};
  }
}
