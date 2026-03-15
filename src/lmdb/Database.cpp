/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <rs/lmdb/Database.h>
#include <rs/lmdb/Type.h>

#include <lmdb.h>

namespace rs::lmdb
{
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

  Reader Database::reader(ReadTransaction& txn) const { return Reader{_dbi, txn._handle.get()}; }

  Writer Database::writer(WriteTransaction& txn) { return Writer{_dbi, txn}; }

  Reader::Reader(MDB_dbi dbi, MDB_txn* txn) : _dbi{dbi}, _txn{txn} {}

  Reader::Iterator Reader::begin() const
  {
    MDB_cursor* cursor = nullptr;
    throwOnError("mdb_cursor_open", mdb_cursor_open(_txn, _dbi, &cursor));
    return Iterator{cursor};
  }

  Reader::Iterator Reader::end() const { return Iterator{}; }

  boost::asio::const_buffer Reader::operator[](std::uint64_t id) const
  {
    MDB_val key{sizeof(id), &id};
    MDB_val value{0, nullptr};
    int const rc = mdb_get(_txn, _dbi, &key, &value);
    if (rc == MDB_NOTFOUND)
    {
      return boost::asio::const_buffer{};
    }
    throwOnError("mdb_get", rc);
    return boost::asio::buffer(value.mv_data, value.mv_size);
  }

  Reader::Iterator::Iterator() : _value{} {}

  Reader::Iterator::Iterator(MDB_cursor* cursor) : _cursor{cursor} { increment(); }

  Reader::Iterator::~Iterator() = default;

  Reader::Iterator::Iterator(Iterator const& other) : _value{other._value}
  {
    if (other._cursor)
    {
      MDB_cursor* cursor = nullptr;
      throwOnError("mdb_cursor_open",
                   mdb_cursor_open(mdb_cursor_txn(other._cursor.get()), mdb_cursor_dbi(other._cursor.get()), &cursor));
      _cursor.reset(cursor);
      MDB_val keyValue{sizeof(std::uint64_t), const_cast<std::uint64_t*>(&_value.first)};
      throwOnError("mdb_cursor_get", mdb_cursor_get(_cursor.get(), &keyValue, nullptr, MDB_SET));
    }
  }

  Reader::Iterator::Iterator(Iterator&& other) noexcept : _cursor{std::move(other._cursor)}, _value{other._value} {}

  bool Reader::Iterator::equal(Iterator const& other) const
  {
    return (_cursor != nullptr) == (other._cursor != nullptr) && _value.first == other._value.first;
  }

  void Reader::Iterator::increment()
  {
    MDB_val keyValue{0, nullptr};
    MDB_val valueBuffer{0, nullptr};

    int const rc = mdb_cursor_get(_cursor.get(), &keyValue, &valueBuffer, MDB_NEXT);
    if (rc == MDB_NOTFOUND)
    {
      _value = Value{};
      _cursor.reset();
    }
    else
    {
      throwOnError("mdb_cursor_get", rc);
      std::string_view key{static_cast<char const*>(keyValue.mv_data), keyValue.mv_size};
      std::string_view value{static_cast<char const*>(valueBuffer.mv_data), valueBuffer.mv_size};
      _value.first = lmdb::read<std::uint64_t>(key);
      _value.second = boost::asio::buffer(value.data(), value.size());
    }
  }

  Reader::Value const& Reader::Iterator::dereference() const { return _value; }

  Writer::Writer(MDB_dbi dbi, WriteTransaction& txn) : _dbi{dbi}, _txn{txn}
  {
    MDB_cursor* cursor = nullptr;
    throwOnError("mdb_cursor_open", mdb_cursor_open(txn._handle.get(), _dbi, &cursor));
    _cursor.reset(cursor);
    MDB_val keyValue{0, nullptr};
    int const rc = mdb_cursor_get(_cursor.get(), &keyValue, nullptr, MDB_LAST);
    if (rc == MDB_SUCCESS)
    {
      std::string_view key{static_cast<char const*>(keyValue.mv_data), keyValue.mv_size};
      _lastId = lmdb::read<std::uint64_t>(key);
    }
    else if (rc != MDB_NOTFOUND)
    {
      throwOnError("mdb_cursor_get", rc);
    }
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
    std::string_view toStrView(boost::asio::const_buffer data)
    {
      return {static_cast<char const*>(data.data()), data.size()};
    }

    void const* put(MDB_cursor* cursor, std::uint64_t id, boost::asio::const_buffer data, unsigned int flags)
    {
      auto key = lmdb::bytesOf(id);
      auto value = toStrView(data);

      MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
      MDB_val valueBuffer{value.size(), const_cast<char*>(value.data())};

      int const rc = mdb_cursor_put(cursor, &keyValue, &valueBuffer, flags);
      if (rc == MDB_KEYEXIST)
      {
        return nullptr;
      }
      throwOnError("mdb_cursor_put", rc);

      // Get the actual stored value
      if (mdb_cursor_get(cursor, &keyValue, &valueBuffer, MDB_GET_CURRENT) != MDB_SUCCESS)
      {
        return nullptr;
      }

      return valueBuffer.mv_data;
    }

    void* reserve(MDB_cursor* cursor, std::uint64_t id, std::size_t size, unsigned int flags)
    {
      auto key = lmdb::bytesOf(id);
      MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
      MDB_val valueBuffer{size, nullptr};
      throwOnError("mdb_cursor_put", mdb_cursor_put(cursor, &keyValue, &valueBuffer, flags | MDB_RESERVE));
      return valueBuffer.mv_data;
    }
  }

  void const* Writer::create(std::uint64_t id, boost::asio::const_buffer data)
  {
    return put(_cursor.get(), id, data, MDB_NOOVERWRITE);
  }

  void* Writer::create(std::uint64_t id, std::size_t size) { return reserve(_cursor.get(), id, size, MDB_NOOVERWRITE); }

  std::pair<std::uint64_t, void const*> Writer::append(boost::asio::const_buffer data)
  {
    auto id = ++_lastId;
    return {id, put(_cursor.get(), id, data, MDB_NOOVERWRITE | MDB_APPEND)};
  }

  std::pair<std::uint64_t, void*> Writer::append(std::size_t size)
  {
    auto id = ++_lastId;
    return {id, reserve(_cursor.get(), id, size, MDB_NOOVERWRITE | MDB_APPEND)};
  }

  void const* Writer::update(std::uint64_t id, boost::asio::const_buffer data)
  {
    return put(_cursor.get(), id, data, 0);
  }

  bool Writer::del(std::uint64_t id)
  {
    MDB_val key{sizeof(id), const_cast<std::uint64_t*>(&id)};
    int const rc = mdb_del(_txn._handle.get(), _dbi, &key, nullptr);
    if (rc == MDB_NOTFOUND)
    {
      return false;
    }
    throwOnError("mdb_del", rc);
    return true;
  }

  boost::asio::const_buffer Writer::operator[](std::uint64_t id) const
  {
    MDB_val key{sizeof(id), &id};
    MDB_val value{0, nullptr};
    int const rc = mdb_get(_txn._handle.get(), _dbi, &key, &value);
    if (rc == MDB_NOTFOUND)
    {
      return boost::asio::const_buffer{};
    }
    throwOnError("mdb_get", rc);
    return boost::asio::buffer(value.mv_data, value.mv_size);
  }
}
