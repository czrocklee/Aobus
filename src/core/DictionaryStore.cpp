// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/DictionaryStore.h>

#include <rs/utility/ByteView.h>
#include <span>
#include <string_view>

namespace rs::core
{
  // Initial capacity for dictionary entries (suitable for most music libraries)
  constexpr std::uint32_t kInitialCapacity = 4096;

  DictionaryStore::DictionaryStore(lmdb::WriteTransaction& txn, std::string const& db) : _database{txn, db}
  {
    // Reserve initial capacity to avoid frequent resizes
    _idToStringStorage.reserve(kInitialCapacity);

    for (auto [id, buf] : _database.reader(txn))
    {
      std::string_view str = utility::asString(buf);
      _idToStringStorage.emplace_back(str);
      _stringToId.emplace(_idToStringStorage.back(), DictionaryId{id});
    }
  }

  DictionaryId DictionaryStore::put(lmdb::WriteTransaction& txn, std::string_view value)
  {
    // Check in-memory index first
    if (auto it = _stringToId.find(value); it != _stringToId.end())
    {
      return it->second;
    }

    // Not found in memory - append to database
    auto writer = _database.writer(txn);
    auto data = utility::asBytes(value);
    auto id = writer.append(data);
    std::string_view str = utility::asString(data);
    _idToStringStorage.emplace_back(str);
    _stringToId.emplace(_idToStringStorage.back(), DictionaryId{id});
    return DictionaryId{id};
  }

  std::string_view DictionaryStore::get(DictionaryId id) const
  {
    auto idx = id.value();
    if (idx >= _idToStringStorage.size())
    {
      throw std::runtime_error{"Invalid dictionary ID"};
    }

    return _idToStringStorage[idx];
  }

  DictionaryId DictionaryStore::getId(std::string_view str) const
  {
    if (auto it = _stringToId.find(str); it != _stringToId.end())
    {
      return it->second;
    }

    throw std::runtime_error{"String not found in dictionary"};
  }

  bool DictionaryStore::contains(std::string_view str) const
  {
    return _stringToId.contains(str);
  }

} // namespace rs::core
