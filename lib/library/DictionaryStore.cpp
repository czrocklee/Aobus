// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ao/library/DictionaryStore.h"

#include "ao/Exception.h"
#include "ao/Type.h"
#include "ao/lmdb/Database.h"
#include "ao/lmdb/Transaction.h"
#include "ao/utility/ByteView.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace ao::library
{
  // Extra capacity for dictionary entries
  constexpr std::uint32_t kExtraCapacity = 4096;

  DictionaryStore::DictionaryStore(lmdb::Database db, lmdb::ReadTransaction const& txn)
    : _database{std::move(db)}, _stringToId{1024, DictHash{&_idToStringStorage}, DictEqual{&_idToStringStorage}}
  {
    auto const reader = _database.reader(txn);

    _idToStringStorage.reserve(reader.maxKey() + kExtraCapacity);

    std::uint32_t expectedId = 1;

    for (auto const& [id, buf] : reader)
    {
      while (expectedId < id)
      {
        _freeIds.emplace_back(expectedId);
        expectedId++;
      }

      if (id > _idToStringStorage.size())
      {
        _idToStringStorage.resize(id);
      }

      _idToStringStorage[id - 1] = std::string{utility::bytes::stringView(buf)};
      _stringToId.insert(DictionaryId{id});

      expectedId = id + 1;
    }
  }

  DictionaryId DictionaryStore::put(lmdb::WriteTransaction& txn, std::string_view value)
  {
    // Check in-memory index first (includes entries from reserve)
    if (auto it = _stringToId.find(value); it != _stringToId.end())
    {
      if (auto resIt = _reservedStrings.find(*it); resIt != _reservedStrings.end())
      {
        auto writer = _database.writer(txn);
        auto data = utility::bytes::view(value);
        writer.create(it->raw(), data);
        _reservedStrings.erase(resIt);
      }

      return *it;
    }

    // Not found in memory - write with ID that avoids getOrIntern collisions
    auto writer = _database.writer(txn);
    auto data = utility::bytes::view(value);
    auto id = popFreeId();

    if (id == kInvalidDictionaryId)
    {
      id = DictionaryId{std::max(writer.maxKey(), static_cast<std::uint32_t>(_idToStringStorage.size())) + 1};

      if (id.raw() > _idToStringStorage.size())
      {
        _idToStringStorage.resize(id.raw());
      }
    }

    writer.create(id.raw(), data);
    _idToStringStorage[id.raw() - 1] = std::string{utility::bytes::stringView(data)};
    _stringToId.insert(id);
    return id;
  }

  std::string_view DictionaryStore::get(DictionaryId id) const
  {
    auto idx = id.raw();

    // 0 is null/invalid
    if (idx == 0)
    {
      ao::throwException<Exception>("Invalid dictionary ID");
    }

    if (idx - 1 >= _idToStringStorage.size())
    {
      ao::throwException<Exception>("Invalid dictionary ID");
    }

    return _idToStringStorage[idx - 1];
  }

  std::string_view DictionaryStore::getOrDefault(DictionaryId id, std::string_view defaultValue) const
  {
    auto idx = id.raw();

    if (idx == 0 || idx - 1 >= _idToStringStorage.size())
    {
      return defaultValue;
    }

    return _idToStringStorage[idx - 1];
  }

  DictionaryId DictionaryStore::getId(std::string_view str) const
  {
    if (auto it = _stringToId.find(str); it != _stringToId.end())
    {
      return *it;
    }

    ao::throwException<Exception>("String not found in dictionary");
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
      return *it;
    }

    // Add to in-memory storage - ID is 1-indexed (0 = null)
    auto id = popFreeId();

    if (id != kInvalidDictionaryId)
    {
      _idToStringStorage[id.raw() - 1] = str;
    }
    else
    {
      _idToStringStorage.emplace_back(str);
      id = DictionaryId{static_cast<std::uint32_t>(_idToStringStorage.size())};
    }

    _reservedStrings.insert(id);
    _stringToId.insert(id);
    return id;
  }
} // namespace ao::library
