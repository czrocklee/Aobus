// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/DictionaryStore.h>

#include <ao/Exception.h>
#include <ao/utility/ByteView.h>
#include <span>
#include <string_view>

namespace ao::library
{
  // Extra capacity for dictionary entries
  constexpr std::uint32_t kExtraCapacity = 4096;

  DictionaryStore::DictionaryStore(lmdb::Database db, lmdb::ReadTransaction& txn)
    : _database{std::move(db)}
  {
    auto const reader = _database.reader(txn);
    _idToStringStorage.reserve(reader.maxKey() + kExtraCapacity);

    for (auto const& [id, buf] : reader)
    {
      auto const& str = _idToStringStorage.emplace_back(utility::bytes::stringView(buf));
      _stringToId.emplace(str, DictionaryId{id});
    }
  }

  DictionaryId DictionaryStore::put(lmdb::WriteTransaction& txn, std::string_view value)
  {
    // Check in-memory index first (includes entries from reserve)

    if (auto it = _stringToId.find(value); it != _stringToId.end())
    {
      // If this string was reserved, we need to persist it now

      if (_reservedStrings.contains(value))
      {
        auto writer = _database.writer(txn);
        auto data = utility::bytes::view(value);
        writer.create(it->second.value(), data);
        _reservedStrings.erase(value);
      }

      return it->second;
    }

    // Not found in memory - append to database
    auto writer = _database.writer(txn);
    auto data = utility::bytes::view(value);
    auto id = writer.append(data);
    auto const& str = _idToStringStorage.emplace_back(utility::bytes::stringView(data));
    _stringToId.emplace(str, DictionaryId{id});
    return DictionaryId{id};
  }

  std::string_view DictionaryStore::get(DictionaryId id) const
  {
    auto idx = id.value();

    // 0 is null/invalid

    if (idx == 0)
    {
      AO_THROW(ao::Exception, "Invalid dictionary ID");
    }

    if (idx - 1 >= _idToStringStorage.size())
    {
      AO_THROW(ao::Exception, "Invalid dictionary ID");
    }

    return _idToStringStorage[idx - 1];
  }

  DictionaryId DictionaryStore::getId(std::string_view str) const
  {
    if (auto it = _stringToId.find(str); it != _stringToId.end())
    {
      return it->second;
    }

    AO_THROW(ao::Exception, "String not found in dictionary");
  }

  bool DictionaryStore::contains(std::string_view str) const
  {
    return _stringToId.contains(str);
  }

  DictionaryId DictionaryStore::getOrIntern(std::string_view str)
  {
    // Check if already exists

    if (auto it = _stringToId.find(str); it != _stringToId.end())
    {
      return it->second;
    }

    // Add to in-memory storage - ID is 1-indexed (0 = null)
    _idToStringStorage.emplace_back(str);
    auto const id = DictionaryId{static_cast<std::uint32_t>(_idToStringStorage.size())};
    _stringToId.emplace(_idToStringStorage.back(), id);
    _reservedStrings.emplace(_idToStringStorage.back());
    return id;
  }
} // namespace ao::library
