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
#include <rs/common/utility/TaggedInteger.h>
#include <boost/iterator/transform_iterator.hpp>

namespace rs::ml::core
{
  template<typename T>
  class FlatBuffersStore
  {
    template<typename N>
    struct IdTag
    {};

  public:
    class Reader;
    class Writer;
    using Id = common::utility::TaggedInteger<std::uint64_t, IdTag<T>>;

    FlatBuffersStore(lmdb::env& env, const std::string& db) : _database{env, db} {}

    Reader reader(LMDBReadTransaction& txn) const { return Reader{_database.reader(txn)}; }
    Writer writer(LMDBWriteTransaction& txn) { return Writer{_database.writer(txn)}; }

  private:
    LMDBDatabase _database;
  };

  template<typename T>
  class FlatBuffersStore<T>::Reader
  {
  public:
    auto begin() const { return boost::make_transform_iterator(_reader.begin(), decode); }
    auto end() const { return boost::make_transform_iterator(_reader.end(), decode); }
    const T* operator[](Id id) const { return ::flatbuffers::GetRoot<T>(_reader[id].data()); }

  private:
    Reader(LMDBDatabase::Reader&& reader) : _reader{std::move(reader)} {}
    static std::pair<Id, const T*> decode(LMDBDatabase::Reader::Value value);
    LMDBDatabase::Reader _reader;
    friend class FlatBuffersStore;
  };

  template<typename T>
  class FlatBuffersStore<T>::Writer
  {
  public:
    using NativeType = typename T::NativeTableType;

    template<typename Builder>
    std::pair<Id, const T*> create(Builder&& builder);
    std::pair<Id, const T*> createT(const NativeType& tt);
    bool del(Id id) { return _writer.del(id); }

  private:
    explicit Writer(LMDBDatabase::Writer&& writer) : _writer{std::move(writer)} {}

    LMDBDatabase::Writer _writer;
    flatbuffers::FlatBufferBuilder _fbb;
    friend class FlatBuffersStore;
  };

  template<typename T>
  inline std::pair<typename FlatBuffersStore<T>::Id, const T*> FlatBuffersStore<T>::Reader::decode(LMDBDatabase::Reader::Value value)
  {
    return {Id{value.first}, ::flatbuffers::GetRoot<T>(value.second.data())};
  }

  template<typename T>
  template<typename Builder>
  inline std::pair<typename FlatBuffersStore<T>::Id, const T*> FlatBuffersStore<T>::Writer::create(Builder&& builder)
  {
    struct BuilderGuard
    {
      flatbuffers::FlatBufferBuilder& fbb;
      ~BuilderGuard() { fbb.Clear(); }
    };

    BuilderGuard guard{_fbb};
    _fbb.Finish(std::invoke(std::forward<Builder>(builder), _fbb));
    auto [id, buffer] = _writer.append(boost::asio::buffer(_fbb.GetBufferPointer(), _fbb.GetSize()));
    return {Id{id}, ::flatbuffers::GetRoot<T>(buffer)};
  }

  template<typename T>
  inline std::pair<typename FlatBuffersStore<T>::Id, const T*> FlatBuffersStore<T>::Writer::createT(const NativeType& tt)
  {
    return create([&tt](flatbuffers::FlatBufferBuilder& fbb) { return T::Pack(fbb, &tt); });
  }
}
