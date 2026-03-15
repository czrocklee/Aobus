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
#include <rs/core/TrackStore.h>

namespace rs::core
{

  // TrackStore implementation
  TrackStore::TrackStore(lmdb::Environment& env, std::string const& db) : _database{env, db} {}

  TrackStore::Reader TrackStore::reader(lmdb::ReadTransaction& txn) const { return Reader{_database.reader(txn)}; }

  TrackStore::Writer TrackStore::writer(lmdb::WriteTransaction& txn) { return Writer{_database.writer(txn)}; }

  // TrackStore::Reader implementation
  TrackStore::Reader::Reader(lmdb::Database::Reader&& reader) : _reader{reader} {}

  TrackStore::Reader::Iterator TrackStore::Reader::begin() const
  {
    auto iter = _reader.begin();
    if (iter != _reader.end())
    {
      [[maybe_unused]] auto&& [id, buffer] = *iter;
      return Iterator{std::move(iter)};
    }
    return end();
  }

  TrackStore::Reader::Iterator TrackStore::Reader::end() const { return Iterator{_reader.end()}; }

  TrackView TrackStore::Reader::operator[](Id id) const
  {
    auto buffer = _reader[id.value()];
    if (buffer.size() == 0)
    {
      return TrackView{};
    }
    return TrackView(buffer.data(), buffer.size());
  }

  // TrackStore::Reader::Iterator implementation
  TrackStore::Reader::Iterator::Iterator(lmdb::Database::Reader::Iterator&& iter) : _iter{std::move(iter)} {}

  bool TrackStore::Reader::Iterator::operator==(Iterator const& other) const { return _iter == other._iter; }

  TrackStore::Reader::Iterator& TrackStore::Reader::Iterator::operator++()
  {
    ++_iter;
    return *this;
  }

  TrackStore::Reader::Iterator::value_type TrackStore::Reader::Iterator::operator*() const
  {
    auto&& [id, buffer] = *_iter;
    return {Id{id}, TrackView(buffer.data(), buffer.size())};
  }

  // TrackStore::Writer implementation
  TrackStore::Writer::Writer(lmdb::Database::Writer&& writer) : _writer{std::move(writer)} {}

  std::pair<TrackStore::Id, TrackView> TrackStore::Writer::create(void const* data, std::size_t size)
  {
    auto [id, buffer] = _writer.append(boost::asio::buffer(data, size));
    return {Id{id}, TrackView{buffer, size}};
  }

  TrackView TrackStore::Writer::update(Id id, void const* data, std::size_t size)
  {
    auto buffer = _writer.update(id.value(), boost::asio::buffer(data, size));
    return TrackView(buffer, size);
  }

  bool TrackStore::Writer::del(Id id) { return _writer.del(id.value()); }

  TrackView TrackStore::Writer::operator[](Id id) const
  {
    auto buffer = _writer[id.value()];
    if (buffer.size() == 0)
    {
      return TrackView{};
    }
    return TrackView(buffer.data(), buffer.size());
  }

} // namespace rs::core
