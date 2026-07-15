// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Database.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>

namespace ao::library
{
  ListStore::ListStore(lmdb::Database db, detail::LibraryIdentity const& identity)
    : _database{std::move(db)}, _identity{&identity}
  {
  }

  ListStore::Reader ListStore::reader(ReadTransaction const& transaction) const
  {
    return Reader{_database.reader(transaction.native(*_identity))};
  }

  ListStore::Reader ListStore::reader(WriteTransaction const& transaction) const
  {
    return Reader{_database.reader(transaction.native(*_identity))};
  }

  ListStore::Writer ListStore::writer(WriteTransaction& transaction) const
  {
    return Writer{_database.writer(transaction.native(*_identity))};
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

    return Iterator{};
  }

  std::optional<ListView> ListStore::Reader::get(ListId id) const
  {
    auto optBytes = _reader.get(id.raw());

    if (!optBytes)
    {
      return std::nullopt;
    }

    return ListView{*optBytes};
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
    return {ListId{id}, ListView{buffer}};
  }

  // Writer implementation
  ListStore::Writer::Writer(lmdb::Database::Writer&& writer)
    : _writer{std::move(writer)}
  {
  }

  Result<std::pair<ListId, ListView>> ListStore::Writer::create(std::span<std::byte const> data)
  {
    auto idResult = _writer.append(data);

    if (!idResult)
    {
      return std::unexpected{idResult.error()};
    }

    auto id = *idResult;
    return std::pair{ListId{id}, ListView{data}};
  }

  Result<> ListStore::Writer::update(ListId id, std::span<std::byte const> data)
  {
    return _writer.update(id.raw(), data);
  }

  bool ListStore::Writer::remove(ListId id)
  {
    return _writer.del(id.raw());
  }

  Result<> ListStore::Writer::clear()
  {
    return _writer.clear();
  }

  std::optional<ListView> ListStore::Writer::get(ListId id) const
  {
    auto optBytes = _writer.get(id.raw());

    if (!optBytes)
    {
      return std::nullopt;
    }

    return ListView{*optBytes};
  }
} // namespace ao::library
