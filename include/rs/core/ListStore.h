/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <rs/core/ListLayout.h>
#include <rs/lmdb/Database.h>
#include <rs/utility/TaggedInteger.h>

#include <boost/iterator/transform_iterator.hpp>
#include <vector>

namespace rs::core
{

  /**
   * ListStore - Binary storage for lists using ListLayout.
   */
  class ListStore
  {
  public:
    struct IdTag
    {};

    using Id = utility::TaggedInteger<std::uint64_t, IdTag>;

    class Reader;
    class Writer;

    ListStore(lmdb::WriteTransaction& txn, std::string const& db);

    Reader reader(lmdb::ReadTransaction& txn) const;
    Writer writer(lmdb::WriteTransaction& txn);

  private:
    lmdb::Database _database;
  };

  /**
   * ListStore::Reader - Read-only access to lists.
   */
  class ListStore::Reader
  {
  public:
    class Iterator;

    [[nodiscard]] Iterator begin() const;
    [[nodiscard]] Iterator end() const;

    ListView operator[](Id id) const;

  private:
    Reader(lmdb::Database::Reader&& reader);

    lmdb::Database::Reader _reader;
    friend class ListStore;
  };

  /**
   * ListStore::Reader::Iterator - Iterator over lists.
   */
  class ListStore::Reader::Iterator
  {
  public:
    using value_type = std::pair<Id, ListView>;

    Iterator() = default;
    Iterator(Iterator const& other) = default;

    bool operator==(Iterator const& other) const;
    Iterator& operator++();
    value_type operator*() const;

  private:
    Iterator(lmdb::Database::Reader::Iterator&& iter);

    lmdb::Database::Reader::Iterator _iter;
    ListView _view;
    friend class Reader;
  };

  /**
   * ListStore::Writer - Write access to lists.
   */
  class ListStore::Writer
  {
  public:
    Writer() = default;

    std::pair<Id, ListView> create(void const* data, std::size_t size);
    ListView update(Id id, void const* data, std::size_t size);
    bool del(Id id);

    ListView operator[](Id id) const;

  private:
    explicit Writer(lmdb::Database::Writer&& writer);

    lmdb::Database::Writer _writer;
    friend class ListStore;
  };

} // namespace rs::core
