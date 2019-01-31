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
  using WriteTransaction = LMDBDatabase::WriteTransaction;
  using ReadTransaction = LMDBDatabase::ReadTransaction;

  struct LMDBDatabase::Impl
  {
    Impl(lmdb::env& env) : env{env} {}
    lmdb::env& env;
    lmdb::dbi dbi = {MDB_dbi{}};
    std::atomic<uint64_t> nextId;
  };

  LMDBDatabase::LMDBDatabase(lmdb::env& env, const std::string& db)
    : _impl{std::make_unique<Impl>(env)}
  {
    auto txn = lmdb::txn::begin(_impl->env);
    _impl->dbi = lmdb::dbi::open(txn, db.c_str(), MDB_CREATE | MDB_INTEGERKEY);

    lmdb::val key;
    auto cursor = lmdb::cursor::open(txn, _impl->dbi);
    _impl->nextId = cursor.get(key, MDB_LAST) ? *key.data<std::uint64_t>() + 1 : 1;

    txn.commit();
  }

  LMDBDatabase::~LMDBDatabase() = default;

  ReadTransaction LMDBDatabase::readTransaction() const
  {
    return ReadTransaction{*_impl};
  }

  WriteTransaction LMDBDatabase::writeTransaction()
  {
    return WriteTransaction{*_impl};
  }

  ReadTransaction::ReadTransaction(LMDBDatabase::Impl& impl)
    : _dbi{impl.dbi}, 
      _txn{lmdb::txn::begin(impl.env, nullptr, MDB_RDONLY)}
  {
  }

  ReadTransaction::Iterator ReadTransaction::begin() const
  {
    return Iterator{lmdb::cursor::open(_txn, _dbi)};
  }

  ReadTransaction::Iterator ReadTransaction::end() const
  {
    return Iterator{};
  }

  boost::asio::const_buffer ReadTransaction::operator[](std::uint64_t id) const
  {
    lmdb::val key{&id, sizeof(id)};
    lmdb::val value;
    return _dbi.get(_txn, key, value) ? boost::asio::buffer(value.data(), value.size()) : boost::asio::const_buffer{};
  }

  ReadTransaction::Iterator::Iterator()
    : _cursor{nullptr}, _value{}
  {
  }

  ReadTransaction::Iterator::Iterator(lmdb::cursor&& cursor)
    : _cursor{std::move(cursor)}
  {
    increment();
  }

  ReadTransaction::Iterator::Iterator(const Iterator& other)
    : _cursor{nullptr}, _value{other._value}
  {
    if (other._cursor != nullptr)
    {
      _cursor = lmdb::cursor::open(other._cursor.txn(), other._cursor.dbi());
      _cursor.find(_value.first);
    }
  }

  ReadTransaction::Iterator::Iterator(Iterator&& other) = default;

  bool ReadTransaction::Iterator::equal(const Iterator& other) const
  {
    return _cursor == other._cursor && _value.first == other._value.first;
  }


  void ReadTransaction::Iterator::increment()
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

  const ReadTransaction::Value& ReadTransaction::Iterator::dereference() const
  {
    return _value;
  }

  WriteTransaction::WriteTransaction(LMDBDatabase::Impl& impl)
    : _impl{impl},
      _txn{lmdb::txn::begin(impl.env)}
    
  {
  }

  std::uint64_t WriteTransaction::create(boost::asio::const_buffer buffer)
  {
    std::uint64_t id = _impl.nextId.fetch_add(1, std::memory_order_relaxed);
    lmdb::val key{&id, sizeof(id)};
    lmdb::val value{buffer.data(), buffer.size()};
    _impl.dbi.put(_txn, key, value, MDB_NOOVERWRITE | MDB_APPEND);
    return id;
  }

  bool WriteTransaction::update(std::uint64_t id, boost::asio::const_buffer buffer)
  {
    lmdb::val key{&id, sizeof(id)};
    lmdb::val value{buffer.data(), buffer.size()};
    return _impl.dbi.put(_txn, key, value);
  }

  bool WriteTransaction::del(std::uint64_t id)
  {
    return _impl.dbi.del(_txn, id);
  }

  void WriteTransaction::commit()
  {
    _txn.commit();
  }

}

