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

#include <boost/asio/buffer.hpp>
#include <rs/core/ListStore.h>

namespace rs::core
{

  ListStore::ListStore(lmdb::Environment& env, const std::string& db) : _database{env, db} {}

  ListStore::Reader ListStore::reader(lmdb::ReadTransaction& txn) const { return Reader{_database.reader(txn)}; }

  ListStore::Writer ListStore::writer(lmdb::WriteTransaction& txn) { return Writer{_database.writer(txn)}; }

  // Reader implementation
  ListStore::Reader::Reader(lmdb::Database::Reader&& reader) : _reader{reader} {}

  ListStore::Reader::Iterator ListStore::Reader::begin() const
  {
    auto iter = _reader.begin();
    if (iter != _reader.end())
    {
      return Iterator{std::move(iter)};
    }
    return end();
  }

  ListStore::Reader::Iterator ListStore::Reader::end() const { return Iterator{_reader.end()}; }

  ListView ListStore::Reader::operator[](Id id) const
  {
    auto buffer = _reader[id.value()];
    if (buffer.size() == 0)
    {
      return ListView{};
    }
    return ListView(buffer.data(), buffer.size());
  }

  // Iterator implementation
  ListStore::Reader::Iterator::Iterator(lmdb::Database::Reader::Iterator&& iter) : _iter{std::move(iter)} {}

  bool ListStore::Reader::Iterator::operator==(const Iterator& other) const { return _iter == other._iter; }

  ListStore::Reader::Iterator& ListStore::Reader::Iterator::operator++()
  {
    ++_iter;
    return *this;
  }

  ListStore::Reader::Iterator::value_type ListStore::Reader::Iterator::operator*() const
  {
    auto&& [id, buffer] = *_iter;
    return {Id{id}, ListView(buffer.data(), buffer.size())};
  }

  // Writer implementation
  ListStore::Writer::Writer(lmdb::Database::Writer&& writer) : _writer{std::move(writer)} {}

  std::pair<ListStore::Id, ListView> ListStore::Writer::create(const void* data, std::size_t size)
  {
    auto [id, buffer] = _writer.append(boost::asio::buffer(data, size));
    return {Id{id}, ListView(buffer, size)};
  }

  ListView ListStore::Writer::update(Id id, const void* data, std::size_t size)
  {
    auto buffer = _writer.update(id.value(), boost::asio::buffer(data, size));
    return ListView(buffer, size);
  }

  bool ListStore::Writer::del(Id id) { return _writer.del(id.value()); }

  ListView ListStore::Writer::operator[](Id id) const
  {
    auto buffer = _writer[id.value()];
    if (buffer.size() == 0)
    {
      return ListView{};
    }
    return ListView(buffer.data(), buffer.size());
  }

} // namespace rs::core
