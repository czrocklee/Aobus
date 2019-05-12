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

#pragma once

#include <rs/ml/core/LMDBDatabase.h>
#include <boost/iterator/transform_iterator.hpp>

namespace rs::ml::core
{
  template<typename T>
  class FlatBuffersStore
  {
  public:
    class ReadTransaction;
    class WriteTransaction;
    using Id = typename T::Id;

    FlatBuffersStore(lmdb::env& env, const std::string& db) : _database{env, db} {}

    ReadTransaction readTransaction() const { return ReadTransaction{_database.readTransaction()}; }
    WriteTransaction writeTransaction() { return WriteTransaction{_database.writeTransaction()}; }

  private:
    LMDBDatabase _database;
  };


  template<typename T>
  class FlatBuffersStore<T>::ReadTransaction
  {
  public:
    auto begin() const { return boost::make_transform_iterator(_txn.begin(), decode); }
    auto end() const { return boost::make_transform_iterator(_txn.end(), decode); }
    const T operator[](Id id) const { return {id, ::flatbuffers::GetRoot<typename T::Value>(_txn[id].data())}; }

  private:
    ReadTransaction(LMDBDatabase::ReadTransaction&& txn) : _txn{std::move(txn)} {}
    static const T decode(LMDBDatabase::ReadTransaction::Value value); 
    LMDBDatabase::ReadTransaction _txn;
    friend class FlatBuffersStore;
  };


  template<typename T>
  class FlatBuffersStore<T>::WriteTransaction
  {
  public:
    template<typename Creator>
    const Id create(Creator&& creator);
    bool del(Id id) { return _txn.del(id); }
    void commit() { _txn.commit(); }
  
  private:
    explicit WriteTransaction(LMDBDatabase::WriteTransaction&& txn) : _txn{std::move(txn)} {}

    LMDBDatabase::WriteTransaction _txn;
    flatbuffers::FlatBufferBuilder _fbb;
    friend class FlatBuffersStore;
  };
  
  template<typename T>
  inline const T FlatBuffersStore<T>::ReadTransaction::decode(LMDBDatabase::ReadTransaction::Value value)
  {
    return {Id{value.first}, ::flatbuffers::GetRoot<typename T::Value>(value.second.data())}; 
  }

  template<typename T>
  template<typename Creator>
  inline const typename T::Id FlatBuffersStore<T>::WriteTransaction::create(Creator&& creator)
  {
    struct BuilderGuard
    {
      flatbuffers::FlatBufferBuilder& fbb;
      ~BuilderGuard() { fbb.Clear(); }
    };

    BuilderGuard guard{_fbb};
    _fbb.Finish(std::invoke(std::forward<Creator>(creator), _fbb));
    return Id{_txn.create(boost::asio::buffer(_fbb.GetBufferPointer(), _fbb.GetSize()))};
  }
}
