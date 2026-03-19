// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/ListStore.h>

namespace rs::core
{

  ListStore::ListStore(lmdb::WriteTransaction& txn, std::string const& db) : _database{txn, db} {}

  ListStore::Reader ListStore::reader(lmdb::ReadTransaction& txn) const { return Reader{_database.reader(txn)}; }

  ListStore::Writer ListStore::writer(lmdb::WriteTransaction& txn) { return Writer{_database.writer(txn)}; }

  // Reader implementation
  ListStore::Reader::Reader(lmdb::Database::Reader&& reader) : _reader{reader} {}

  ListStore::Reader::Iterator ListStore::Reader::begin() const
  {
    if (auto iter = _reader.begin(); iter != _reader.end())
    {
      return Iterator{std::move(iter)};
    }
    return end();
  }

  ListStore::Reader::Iterator ListStore::Reader::end() const { return Iterator{_reader.end()}; }

  std::optional<ListView> ListStore::Reader::get(ListId id) const
  {
    auto optBuffer = _reader.get(id.value());
    if (!optBuffer || optBuffer->size() == 0)
    {
      return std::nullopt;
    }
    return ListView{*optBuffer};
  }

  // Iterator implementation
  ListStore::Reader::Iterator::Iterator(lmdb::Database::Reader::Iterator&& iter) : _iter{std::move(iter)} {}

  bool ListStore::Reader::Iterator::operator==(Iterator const& other) const { return _iter == other._iter; }

  ListStore::Reader::Iterator& ListStore::Reader::Iterator::operator++()
  {
    ++_iter;
    return *this;
  }

  ListStore::Reader::Iterator::value_type ListStore::Reader::Iterator::operator*() const
  {
    auto&& [id, buffer] = *_iter;
    return {ListId{id}, ListView(buffer)};
  }

  // Writer implementation
  ListStore::Writer::Writer(lmdb::Database::Writer&& writer) : _writer{std::move(writer)} {}

  std::pair<ListId, ListView> ListStore::Writer::create(std::span<std::byte const> data)
  {
    auto [id, buffer] = _writer.append(data);
    return {ListId{id}, ListView{buffer}};
  }

  ListView ListStore::Writer::update(ListId id, std::span<std::byte const> data)
  {
    auto buffer = _writer.update(id.value(), data);
    return ListView{data};
  }

  bool ListStore::Writer::del(ListId id) { return _writer.del(id.value()); }

  std::optional<ListView> ListStore::Writer::get(ListId id) const
  {
    auto optBuffer = _writer.get(id.value());
    if (!optBuffer || optBuffer->size() == 0)
    {
      return std::nullopt;
    }
    return ListView{*optBuffer};
  }

} // namespace rs::core
