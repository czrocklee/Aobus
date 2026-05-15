// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>
#include <ao/Type.h>

#include <cstddef>
#include <optional>
#include <span>
#include <utility>

namespace ao::library
{
  ListStore::ListStore(lmdb::Database db)
    : _database{std::move(db)}
  {
  }

  ListStore::Reader ListStore::reader(lmdb::ReadTransaction const& txn) const
  {
    return Reader{_database.reader(txn)};
  }

  ListStore::Writer ListStore::writer(lmdb::WriteTransaction& txn)
  {
    return Writer{_database.writer(txn)};
  }

  // Reader implementation
  ListStore::Reader::Reader(lmdb::Database::Reader reader)
    : _reader{std::move(reader)}
  {
  }

  ListStore::Reader::Iterator ListStore::Reader::begin() const
  {
    if (auto iter = _reader.begin(); iter != _reader.end())
    {
      return Iterator{std::move(iter)};
    }

    return end();
  }

  ListStore::Reader::Iterator ListStore::Reader::end() const
  {
    return Iterator{_reader.end()};
  }

  std::optional<ListView> ListStore::Reader::get(ListId id) const
  {
    return _reader.get(id.value()).transform([](auto const& buffer) { return ListView{buffer}; });
  }

  // Iterator implementation
  ListStore::Reader::Iterator::Iterator(lmdb::Database::Reader::Iterator&& iter)
    : _iter{std::move(iter)}
  {
  }

  bool ListStore::Reader::Iterator::operator==(Iterator const& other) const
  {
    return _iter == other._iter;
  }

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
  ListStore::Writer::Writer(lmdb::Database::Writer&& writer)
    : _writer{std::move(writer)}
  {
  }

  std::pair<ListId, ListView> ListStore::Writer::create(std::span<std::byte const> data)
  {
    auto id = _writer.append(data);
    return {ListId{id}, ListView{data}};
  }

  void ListStore::Writer::update(ListId id, std::span<std::byte const> data)
  {
    _writer.update(id.value(), data);
  }

  bool ListStore::Writer::del(ListId id)
  {
    return _writer.del(id.value());
  }

  void ListStore::Writer::clear()
  {
    _writer.clear();
  }

  std::optional<ListView> ListStore::Writer::get(ListId id) const
  {
    return _writer.get(id.value()).transform([](auto const& buffer) { return ListView{buffer}; });
  }
} // namespace ao::library
