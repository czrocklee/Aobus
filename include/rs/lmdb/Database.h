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

#include <boost/asio/buffer.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <memory>
#include <rs/lmdb/Transaction.h>
#include <rs/lmdb/Type.h>

namespace rs::lmdb
{
  class Database
  {
  public:
    class Reader;
    class Writer;

    Database(lmdb::Environment& env, std::string const& db);
    ~Database();

    [[nodiscard]] Reader reader(ReadTransaction& txn) const;
    [[nodiscard]] Writer writer(WriteTransaction& txn);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };

  class Database::Reader
  {
  public:
    using Value = std::pair<std::uint64_t, boost::asio::const_buffer>;
    class Iterator;

    [[nodiscard]] Iterator begin() const;
    [[nodiscard]] Iterator end() const;
    [[nodiscard]] boost::asio::const_buffer operator[](std::uint64_t id) const;

  protected:
    Reader(lmdb::MDB& dbi, lmdb::Transaction& txn);

    lmdb::MDB& _dbi;
    lmdb::Transaction& _txn;
    friend class Database;
  };

  class Database::Reader::Iterator : public boost::iterator_facade<Iterator, Value const, boost::forward_traversal_tag>
  {
  public:
    friend class boost::iterator_core_access;

    Iterator();
    Iterator(Iterator const& other);
    Iterator(Iterator&& other);
    [[nodiscard]] bool equal(Iterator const& other) const;
    void increment();
    [[nodiscard]] Value const& dereference() const;

  private:
    Iterator(lmdb::Cursor&& cursor);

    lmdb::Cursor _cursor;
    Value _value;
    friend class Reader;
  };

  class Database::Writer
  {
  public:
    [[nodiscard]] void const* create(std::uint64_t id, boost::asio::const_buffer data);
    [[nodiscard]] void* create(std::uint64_t id, std::size_t size);
    [[nodiscard]] std::pair<std::uint64_t, void const*> append(boost::asio::const_buffer data);
    [[nodiscard]] std::pair<std::uint64_t, void*> append(std::size_t size);
    [[nodiscard]] void const* update(std::uint64_t id, boost::asio::const_buffer data);
    bool del(std::uint64_t id);
    [[nodiscard]] boost::asio::const_buffer operator[](std::uint64_t id) const;

  private:
    Writer(lmdb::MDB& dbi, lmdb::Transaction& txn);

    lmdb::MDB& _dbi;
    lmdb::Transaction& _txn;
    lmdb::Cursor _cursor;
    std::uint64_t _lastId = 0;
    friend class Database;
  };

}
