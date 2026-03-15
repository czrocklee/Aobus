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

#include <lmdb.h>

namespace rs::lmdb
{
  using Writer = Database::Writer;
  using Reader = Database::Reader;

  Database::Database(lmdb::Environment& env, std::string const& db)
  {
    WriteTransaction txn{env};
    throwOnError("mdb_dbi_open", mdb_dbi_open(txn.raw(), db.c_str(), MDB_CREATE | MDB_INTEGERKEY, &_dbi));
    txn.commit();
  }

  Database::~Database() = default;

  Reader Database::reader(ReadTransaction& txn) const { return Reader{_dbi, txn.raw()}; }

  Writer Database::writer(WriteTransaction& txn) { return Writer{_dbi, txn}; }

  Reader::Reader(MDB_dbi dbi, MDB_txn* txn) : _dbi{dbi}, _txn{txn} {}

  Reader::Iterator Reader::begin() const { return Iterator{detail::Cursor::open(_txn, _dbi)}; }

  Reader::Iterator Reader::end() const { return Iterator{}; }

  boost::asio::const_buffer Reader::operator[](std::uint64_t id) const
  {
    MDB_val key{id, const_cast<std::uint64_t*>(&id)};
    MDB_val value{0, nullptr};
    const int rc = mdb_get(_txn, _dbi, &key, &value);
    if (rc == MDB_NOTFOUND)
    {
      return boost::asio::const_buffer{};
    }
    throwOnError("mdb_get", rc);
    return boost::asio::buffer(value.mv_data, value.mv_size);
  }

  Reader::Iterator::Iterator() : _cursor{}, _value{} {}

  Reader::Iterator::Iterator(detail::Cursor&& cursor) : _cursor{std::move(cursor)} { increment(); }

  Reader::Iterator::Iterator(Iterator const& other) : _cursor{}, _value{other._value}
  {
    if (other._cursor.valid())
    {
      _cursor = detail::Cursor::open(other._cursor.transaction(), other._cursor.database());
      std::string_view key = lmdb::bytesOf(_value.first);
      [[maybe_unused]] bool ok = _cursor.get(key, MDB_SET);
    }
  }

  Reader::Iterator::Iterator(Iterator&& other) = default;

  bool Reader::Iterator::equal(Iterator const& other) const
  {
    return _cursor.valid() == other._cursor.valid() && _value.first == other._value.first;
  }

  void Reader::Iterator::increment()
  {
    std::string_view key, value;

    if (!_cursor.get(key, value, MDB_NEXT))
    {
      _value = Value{};
      _cursor.close();
    }
    else
    {
      _value.first = lmdb::read<std::uint64_t>(key);
      _value.second = boost::asio::buffer(value.data(), value.size());
    }
  }

  Reader::Value const& Reader::Iterator::dereference() const { return _value; }

  Writer::Writer(MDB_dbi dbi, WriteTransaction& txn) : _dbi{dbi}, _txn{txn}, _cursor{detail::Cursor::open(txn.raw(), _dbi)}
  {
    std::string_view key;
    _lastId = _cursor.get(key, MDB_LAST) ? lmdb::read<std::uint64_t>(key) : 0;
  }

  namespace
  {
    std::string_view toStrView(boost::asio::const_buffer data)
    {
      return {static_cast<char const*>(data.data()), data.size()};
    }

    void const* put(detail::Cursor& cursor, std::uint64_t id, boost::asio::const_buffer data, unsigned int flags)
    {
      auto key = lmdb::bytesOf(id);
      auto value = toStrView(data);

      if (!cursor.put(key, value, flags)) return nullptr;

      if (!cursor.get(key, value, MDB_GET_CURRENT)) return nullptr;

      return value.data();
    }

    void* reserve(detail::Cursor& cursor, std::uint64_t id, std::size_t size, unsigned int flags)
    {
      return cursor.reserve(lmdb::bytesOf(id), size, flags);
    }
  }

  void const* Writer::create(std::uint64_t id, boost::asio::const_buffer data)
  {
    return put(_cursor, id, data, MDB_NOOVERWRITE);
  }

  void* Writer::create(std::uint64_t id, std::size_t size) { return reserve(_cursor, id, size, MDB_NOOVERWRITE); }

  std::pair<std::uint64_t, void const*> Writer::append(boost::asio::const_buffer data)
  {
    auto id = ++_lastId;
    return {id, put(_cursor, id, data, MDB_NOOVERWRITE | MDB_APPEND)};
  }

  std::pair<std::uint64_t, void*> Writer::append(std::size_t size)
  {
    auto id = ++_lastId;
    return {id, reserve(_cursor, id, size, MDB_NOOVERWRITE | MDB_APPEND)};
  }

  void const* Writer::update(std::uint64_t id, boost::asio::const_buffer data) { return put(_cursor, id, data, 0); }

  bool Writer::del(std::uint64_t id)
  {
    MDB_val key{sizeof(id), const_cast<std::uint64_t*>(&id)};
    const int rc = mdb_del(_txn.raw(), _dbi, &key, nullptr);
    if (rc == MDB_NOTFOUND)
    {
      return false;
    }
    throwOnError("mdb_del", rc);
    return true;
  }

  boost::asio::const_buffer Writer::operator[](std::uint64_t id) const
  {
    MDB_val key{id, const_cast<std::uint64_t*>(&id)};
    MDB_val value{0, nullptr};
    const int rc = mdb_get(_txn.raw(), _dbi, &key, &value);
    if (rc == MDB_NOTFOUND)
    {
      return boost::asio::const_buffer{};
    }
    throwOnError("mdb_get", rc);
    return boost::asio::buffer(value.mv_data, value.mv_size);
  }
}
