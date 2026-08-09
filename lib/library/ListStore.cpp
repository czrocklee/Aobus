// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/ListStore.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListView.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Database.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <algorithm>
#include <expected>
#include <optional>
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

  ListStore::Reader ListStore::reader(LibraryWrite const& write) const
  {
    return Reader{_database.reader(write.native(*_identity))};
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

    auto view = ListView{*optBytes};

    AO_INVARIANT(view.isValid(), "List {} record is structurally corrupt after library validation", id);

    return view;
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
    auto const listId = ListId{id};
    auto view = ListView{buffer};

    AO_INVARIANT(view.isValid(), "List {} record is structurally corrupt after library validation", listId);

    return {listId, view};
  }

  // Writer implementation
  ListStore::Writer::Writer(lmdb::Database::Writer&& writer)
    : _writer{std::move(writer)}
  {
  }

  Result<ListId> ListStore::Writer::create(ListBuilder::Prepared const& prepared)
  {
    auto idRes = _writer.append(prepared.size());

    if (!idRes)
    {
      return std::unexpected{idRes.error()};
    }

    auto const& [rawListId, bytes] = *idRes;
    AO_ENSURES(rawListId != kInvalidListId.raw(), "List key allocation produced the reserved id zero");
    prepared.writeTo(bytes);
    AO_ENSURES(
      std::ranges::equal(bytes, prepared.bytes()), "Prepared List encoder did not reproduce its validated snapshot");
    return ListId{rawListId};
  }

  Result<> ListStore::Writer::update(ListId id, ListBuilder::Prepared const& prepared)
  {
    AO_EXPECTS(id != kInvalidListId, "Cannot update the reserved List id zero");

    auto bytesRes = _writer.update(id.raw(), prepared.size());

    if (!bytesRes)
    {
      return std::unexpected{bytesRes.error()};
    }

    prepared.writeTo(*bytesRes);
    AO_ENSURES(std::ranges::equal(*bytesRes, prepared.bytes()),
               "Prepared List encoder did not reproduce its validated snapshot");
    return {};
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

    auto view = ListView{*optBytes};

    AO_INVARIANT(view.isValid(), "List {} record is structurally corrupt after library validation", id);

    return view;
  }
} // namespace ao::library
