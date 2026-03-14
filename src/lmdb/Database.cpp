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

namespace rs::lmdb
{
  using Writer = Database::Writer;
  using Reader = Database::Reader;

  struct Database::Impl
  {
    Impl(lmdb::Environment& env) : env{env} {}
    lmdb::Environment& env;
    lmdb::MDB dbi;
  };

  Database::Database(lmdb::Environment& env, const std::string& db) : _impl{std::make_unique<Impl>(env)}
  {
    auto txn = lmdb::Transaction::begin(_impl->env);
    _impl->dbi = lmdb::MDB::open(txn, db.c_str(), MDB_CREATE | MDB_INTEGERKEY);
    txn.commit();
  }

  Database::~Database() = default;

  Reader Database::reader(ReadTransaction& txn) const { return Reader{_impl->dbi, txn._txn}; }

  Writer Database::writer(WriteTransaction& txn) { return Writer{_impl->dbi, txn._txn}; }

  Reader::Reader(lmdb::MDB& dbi, lmdb::Transaction& txn) : _dbi{dbi}, _txn{txn} {}

  Reader::Iterator Reader::begin() const { return Iterator{lmdb::Cursor::open(_txn, _dbi)}; }

  Reader::Iterator Reader::end() const { return Iterator{}; }

  boost::asio::const_buffer Reader::operator[](std::uint64_t id) const
  {
    std::string_view value;
    return _dbi.get(_txn, lmdb::bytesOf(id), value) ? boost::asio::buffer(value.data(), value.size())
                                                    : boost::asio::const_buffer{};
  }

  Reader::Iterator::Iterator() : _cursor{}, _value{} {}

  Reader::Iterator::Iterator(lmdb::Cursor&& cursor) : _cursor{std::move(cursor)} { increment(); }

  Reader::Iterator::Iterator(const Iterator& other) : _cursor{}, _value{other._value}
  {
    if (other._cursor.valid())
    {
      _cursor = lmdb::Cursor::open(other._cursor.transaction(), other._cursor.database());
      std::string_view key = lmdb::bytesOf(_value.first);
      _cursor.get(key, MDB_SET);
    }
  }

  Reader::Iterator::Iterator(Iterator&& other) = default;

  bool Reader::Iterator::equal(const Iterator& other) const
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

  const Reader::Value& Reader::Iterator::dereference() const { return _value; }

  Writer::Writer(lmdb::MDB& dbi, lmdb::Transaction& txn) : _dbi{dbi}, _txn{txn}, _cursor{lmdb::Cursor::open(_txn, _dbi)}
  {
    std::string_view key;
    _lastId = _cursor.get(key, MDB_LAST) ? lmdb::read<std::uint64_t>(key) : 0;
  }

  namespace
  {
    std::string_view toStrView(boost::asio::const_buffer data)
    {
      return {static_cast<const char*>(data.data()), data.size()};
    }

    const void* put(lmdb::Cursor& cursor, std::uint64_t id, boost::asio::const_buffer data, unsigned int flags)
    {
      auto key = lmdb::bytesOf(id);
      auto value = toStrView(data);
      cursor.put(key, value, flags);
      cursor.get(key, value, MDB_GET_CURRENT);
      return value.data();
    }

    void* reserve(lmdb::Cursor& cursor, std::uint64_t id, std::size_t size, unsigned int flags)
    {
      return cursor.reserve(lmdb::bytesOf(id), size, flags);
    }
  }

  const void* Writer::create(std::uint64_t id, boost::asio::const_buffer data)
  {
    return put(_cursor, id, data, MDB_NOOVERWRITE);
  }

  void* Writer::create(std::uint64_t id, std::size_t size) { return reserve(_cursor, id, size, MDB_NOOVERWRITE); }

  std::pair<std::uint64_t, const void*> Writer::append(boost::asio::const_buffer data)
  {
    auto id = ++_lastId;
    return {id, put(_cursor, id, data, MDB_NOOVERWRITE | MDB_APPEND)};
  }

  std::pair<std::uint64_t, void*> Writer::append(std::size_t size)
  {
    auto id = ++_lastId;
    return {id, reserve(_cursor, id, size, MDB_NOOVERWRITE | MDB_APPEND)};
  }

  const void* Writer::update(std::uint64_t id, boost::asio::const_buffer data) { return put(_cursor, id, data, 0); }

  bool Writer::del(std::uint64_t id) { return _dbi.del(_txn, lmdb::bytesOf(id)); }

  boost::asio::const_buffer Writer::operator[](std::uint64_t id) const
  {
    std::string_view value;
    return _dbi.get(_txn, lmdb::bytesOf(id), value) ? boost::asio::buffer(value.data(), value.size())
                                                    : boost::asio::const_buffer{};
  }
}
