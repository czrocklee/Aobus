// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackStore.h>

namespace rs::core
{

  // TrackStore implementation
  TrackStore::TrackStore(lmdb::WriteTransaction& txn, std::string const& db) : _database{txn, db} {}

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

  std::optional<TrackView> TrackStore::Reader::get(Id id) const
  {
    auto optBuffer = _reader.get(id.value());
    if (!optBuffer || optBuffer->size() == 0)
    {
      return std::nullopt;
    }
    return TrackView(*optBuffer);
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
    return {TrackStore::Id{(id)}, TrackView(buffer)};
  }

  // TrackStore::Writer implementation
  TrackStore::Writer::Writer(lmdb::Database::Writer&& writer) : _writer{std::move(writer)} {}

  std::pair<TrackStore::Id, TrackView> TrackStore::Writer::create(std::span<std::byte const> data)
  {
    auto [id, buffer] = _writer.append(data);
    return {TrackStore::Id{id}, TrackView{data}};
  }

  TrackView TrackStore::Writer::update(Id id, std::span<std::byte const> data)
  {
    auto buffer = _writer.update(id.value(), data);
    return TrackView(data);
  }

  bool TrackStore::Writer::del(Id id) { return _writer.del(id.value()); }

  std::optional<TrackView> TrackStore::Writer::get(Id id) const
  {
    auto optBuffer = _writer.get(id.value());
    if (!optBuffer || optBuffer->size() == 0)
    {
      return std::nullopt;
    }
    return TrackView(*optBuffer);
  }

} // namespace rs::core
