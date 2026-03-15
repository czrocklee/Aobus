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

namespace rs::lmdb
{
  class Database
  {
  public:
    class Reader;
    class Writer;

    Database() = default;
    Database(WriteTransaction& txn, std::string const& db);
    Database(ReadTransaction& txn, std::string const& db);
    ~Database();

    [[nodiscard]] Reader reader(ReadTransaction& txn) const;
    [[nodiscard]] Writer writer(WriteTransaction& txn);

  private:
    MDB_dbi _dbi = (std::numeric_limits<MDB_dbi>::max)();
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
    Reader(MDB_dbi dbi, MDB_txn* txn);

    MDB_dbi _dbi;
    MDB_txn* _txn;
    friend class Database;
  };

  class Database::Reader::Iterator : public boost::iterator_facade<Iterator, Value const, boost::forward_traversal_tag>
  {
  public:
    friend class boost::iterator_core_access;

    Iterator();
    Iterator(Iterator const& other);
    Iterator(Iterator&& other) noexcept;
    ~Iterator();
    [[nodiscard]] bool equal(Iterator const& other) const;
    void increment();
    [[nodiscard]] Value const& dereference() const;

  private:
    struct CursorDeleter { void operator()(MDB_cursor* cur) const { mdb_cursor_close(cur); } };
    Iterator(MDB_cursor* cursor);

    std::unique_ptr<MDB_cursor, CursorDeleter> _cursor;
    Value _value;
    friend class Reader;
  };

  class Database::Writer
  {
  public:
    Writer(Writer&&) noexcept;
    ~Writer();

    [[nodiscard]] void const* create(std::uint64_t id, boost::asio::const_buffer data);
    [[nodiscard]] void* create(std::uint64_t id, std::size_t size);
    [[nodiscard]] std::pair<std::uint64_t, void const*> append(boost::asio::const_buffer data);
    [[nodiscard]] std::pair<std::uint64_t, void*> append(std::size_t size);
    [[nodiscard]] void const* update(std::uint64_t id, boost::asio::const_buffer data);
    bool del(std::uint64_t id);
    [[nodiscard]] boost::asio::const_buffer operator[](std::uint64_t id) const;

  private:
    struct CursorDeleter { void operator()(MDB_cursor* cur) const { mdb_cursor_close(cur); } };
    Writer(MDB_dbi dbi, WriteTransaction& txn);

    MDB_dbi _dbi;
    WriteTransaction& _txn;
    std::unique_ptr<MDB_cursor, CursorDeleter> _cursor;
    std::uint64_t _lastId = 0;
    friend class Database;
  };

}
