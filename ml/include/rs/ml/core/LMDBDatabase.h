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

#include <rs/ml/core/Track.h>
#include <rs/ml/core/UpdateObserver.h>
#include <lmdb++.h>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/asio/buffer.hpp>
#include <mutex>

namespace rs::ml::core
{
  class LMDBDatabase
  {
  public:
    class ReadTransaction;
    class WriteTransaction;

    LMDBDatabase(lmdb::env& env, const std::string& db);
    ~LMDBDatabase();

    ReadTransaction readTransaction() const;
    WriteTransaction writeTransaction();

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };

  class LMDBDatabase::ReadTransaction
  {
  public:
    using Value = std::pair<std::uint64_t, boost::asio::const_buffer>;
    class Iterator;

    Iterator begin() const;
    Iterator end() const;
    boost::asio::const_buffer operator[](std::uint64_t id) const;

  private:
    explicit ReadTransaction(LMDBDatabase::Impl& impl);

    lmdb::dbi& _dbi;
    lmdb::txn _txn;
    friend class LMDBDatabase;
  };

  class LMDBDatabase::ReadTransaction::Iterator : 
    public boost::iterator_facade<Iterator, const Value, boost::forward_traversal_tag>
  {
  public:
    friend class boost::iterator_core_access;

    Iterator();
    Iterator(const Iterator& other);
    Iterator(Iterator&& other);
    bool equal(const Iterator& other) const;
    void increment();
    const Value& dereference() const;

  private:
    Iterator(lmdb::cursor&& cursor);
 
    lmdb::cursor _cursor;
    Value _value;
    friend class ReadTransaction;
  };

  class LMDBDatabase::WriteTransaction
  {
  public:
    std::uint64_t create(boost::asio::const_buffer data);
    bool update(std::uint64_t id, boost::asio::const_buffer data);
    bool del(std::uint64_t id);
    void commit();
  
  private:
    explicit WriteTransaction(LMDBDatabase::Impl& impl);

    LMDBDatabase::Impl& _impl;
    lmdb::txn _txn;

    friend class LMDBDatabase;
  };

}
