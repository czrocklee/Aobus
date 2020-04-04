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

#include <rs/ml/core/LMDBDatabase.h>
#include <atomic>

namespace rs::ml::core
{
  using Writer = LMDBDatabase::Writer;
  using Reader = LMDBDatabase::Reader;

  struct LMDBDatabase::Impl
  {
    Impl(lmdb::env& env) : env{env} {}
    lmdb::env& env;
    lmdb::dbi dbi = {MDB_dbi{}};
  };

  LMDBDatabase::LMDBDatabase(lmdb::env& env, const std::string& db) : _impl{std::make_unique<Impl>(env)}
  {
    auto txn = lmdb::txn::begin(_impl->env);
    _impl->dbi = lmdb::dbi::open(txn, db.c_str(), MDB_CREATE | MDB_INTEGERKEY);
    txn.commit();
  }

  LMDBDatabase::~LMDBDatabase() = default;

  Reader LMDBDatabase::reader(LMDBReadTransaction& txn) const
  {
    return Reader{_impl->dbi, txn._txn};
  }

  Writer LMDBDatabase::writer(LMDBWriteTransaction& txn)
  {
    return Writer{_impl->dbi, txn._txn};
  }

  Reader::Reader(lmdb::dbi& dbi, lmdb::txn& txn) : _dbi{dbi}, _txn{txn} {}

  Reader::Iterator Reader::begin() const
  {
    return Iterator{lmdb::cursor::open(_txn, _dbi)};
  }

  Reader::Iterator Reader::end() const
  {
    return Iterator{};
  }

  boost::asio::const_buffer Reader::operator[](std::uint64_t id) const
  {
    lmdb::val key{&id, sizeof(id)};
    lmdb::val value;
    return _dbi.get(_txn, key, value) ? boost::asio::buffer(value.data(), value.size()) : boost::asio::const_buffer{};
  }

  Reader::Iterator::Iterator() : _cursor{nullptr}, _value{} {}

  Reader::Iterator::Iterator(lmdb::cursor&& cursor) : _cursor{std::move(cursor)}
  {
    increment();
  }

  Reader::Iterator::Iterator(const Iterator& other) : _cursor{nullptr}, _value{other._value}
  {
    if (other._cursor != nullptr)
    {
      _cursor = lmdb::cursor::open(other._cursor.txn(), other._cursor.dbi());
      _cursor.find(_value.first);
    }
  }

  Reader::Iterator::Iterator(Iterator&& other) = default;

  bool Reader::Iterator::equal(const Iterator& other) const
  {
    return _cursor == other._cursor && _value.first == other._value.first;
  }

  void Reader::Iterator::increment()
  {
    lmdb::val key, value;

    if (!_cursor.get(key, value, MDB_NEXT))
    {
      _value = Value{};
      _cursor.close();
    }
    else
    {
      _value.first = *key.data<std::uint64_t>();
      _value.second = boost::asio::buffer(value.data(), value.size());
    }
  }

  const Reader::Value& Reader::Iterator::dereference() const
  {
    return _value;
  }

  Writer::Writer(lmdb::dbi& dbi, lmdb::txn& txn) : _dbi{dbi}, _txn{txn}, _cursor{lmdb::cursor::open(_txn, _dbi)}
  {
    lmdb::val key;
    _cursor.get(key, MDB_LAST) ? *key.data<std::uint64_t>() : 0;
  }

  namespace
  {
    const void* put(lmdb::cursor& cursor, std::uint64_t id, boost::asio::const_buffer data, unsigned int flags)
    {
      lmdb::val key{&id, sizeof(id)};
      lmdb::val value{data.data(), data.size()};
      lmdb::cursor_put(cursor.handle(), key, value, flags);
      cursor.get(key, value, MDB_GET_CURRENT);
      return value.data();
    }

    void* put(lmdb::cursor& cursor, std::uint64_t id, std::size_t size, unsigned int flags)
    {
      lmdb::val key{&id, sizeof(id)};
      lmdb::val value{nullptr, size};
      lmdb::cursor_put(cursor.handle(), key, value, flags | MDB_RESERVE);
      return value.data();
    }
  }

  const void* Writer::create(std::uint64_t id, boost::asio::const_buffer data)
  {
    return put(_cursor, id, data, MDB_NOOVERWRITE);
  }

  void* Writer::create(std::uint64_t id, std::size_t size)
  {
    return put(_cursor, id, size, MDB_NOOVERWRITE);
  }

  std::pair<std::uint64_t, const void*> Writer::append(boost::asio::const_buffer data)
  {
    auto id = ++_lastId;
    return {id, put(_cursor, ++_lastId, data, MDB_NOOVERWRITE | MDB_APPEND)};
  }

  std::pair<std::uint64_t, void*> Writer::append(std::size_t size)
  {
    auto id = ++_lastId;
    return {id, put(_cursor, ++_lastId, size, MDB_NOOVERWRITE | MDB_APPEND)};
  }

  const void* Writer::update(std::uint64_t id, boost::asio::const_buffer data)
  {
    return put(_cursor, id, data, 0);
  }

  bool Writer::del(std::uint64_t id)
  {
    return _dbi.del(_txn, id);
  }
}
