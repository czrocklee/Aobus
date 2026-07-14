// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/DictionaryStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string_view>
#include <utility>

namespace ao::library
{
  DictionaryStore::DictionaryStore(lmdb::Database db, lmdb::ReadTransaction const& transaction)
    : _database{std::move(db)}, _stringToId{0, DictHash{&_idToStringStorage}, DictEqual{&_idToStringStorage}}
  {
    auto const reader = _database.reader(transaction);
    _stringToId.reserve(reader.entryCount());

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

  Result<DictionaryId> DictionaryStore::put(lmdb::WriteTransaction& transaction, std::string_view value)
  {
    auto const lock = std::scoped_lock{_mutex};

    // Check in-memory index first (includes entries from reserve)
    if (auto it = _stringToId.find(value); it != _stringToId.end())
    {
      if (auto resIt = _reservedStrings.find(*it); resIt != _reservedStrings.end())
      {
        auto writer = _database.writer(transaction);
        auto data = utility::bytes::view(value);

        if (auto result = writer.create(it->raw(), data); !result)
        {
          return std::unexpected{result.error()};
        }

        _reservedStrings.erase(resIt);
      }

      return *it;
    }

    // Not found in memory - write with ID that avoids getOrIntern collisions
    auto writer = _database.writer(transaction);
    auto data = utility::bytes::view(value);
    auto id = _freeIds.empty() ? kInvalidDictionaryId : _freeIds.back();

    if (id == kInvalidDictionaryId)
    {
      id = DictionaryId{std::max(writer.maxKey(), static_cast<std::uint32_t>(_idToStringStorage.size())) + 1};
    }

    if (auto result = writer.create(id.raw(), data); !result)
    {
      return std::unexpected{result.error()};
    }

    if (!_freeIds.empty() && _freeIds.back() == id)
    {
      _freeIds.pop_back();
    }

    if (id.raw() > _idToStringStorage.size())
    {
      _idToStringStorage.resize(id.raw());
    }

    _idToStringStorage[id.raw() - 1] = std::string{utility::bytes::stringView(data)};
    _stringToId.insert(id);
    return id;
  }

  std::string_view DictionaryStore::get(DictionaryId id) const
  {
    auto const lock = std::shared_lock{_mutex};
    auto index = id.raw();

    // 0 is null/invalid
    if (index == 0)
    {
      ao::throwException<Exception>("Invalid dictionary ID");
    }

    if (index - 1 >= _idToStringStorage.size())
    {
      ao::throwException<Exception>("Invalid dictionary ID");
    }

    return _idToStringStorage[index - 1];
  }

  std::string_view DictionaryStore::getOrDefault(DictionaryId id, std::string_view defaultValue) const
  {
    auto const lock = std::shared_lock{_mutex};
    auto index = id.raw();

    if (index == 0 || index - 1 >= _idToStringStorage.size())
    {
      return defaultValue;
    }

    return _idToStringStorage[index - 1];
  }

  DictionaryId DictionaryStore::lookupId(std::string_view str) const
  {
    auto const lock = std::shared_lock{_mutex};

    if (auto it = _stringToId.find(str); it != _stringToId.end())
    {
      return *it;
    }

    ao::throwException<Exception>("String not found in dictionary");
  }

  bool DictionaryStore::contains(std::string_view str) const
  {
    auto const lock = std::shared_lock{_mutex};
    return _stringToId.contains(str);
  }

  DictionaryId DictionaryStore::getOrIntern(std::string_view str)
  {
    auto const lock = std::scoped_lock{_mutex};

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

  DictionaryReadCache::DictionaryReadCache(DictionaryStore const& dictionary)
    : _dictionary{&dictionary}
  {
  }

  std::string_view DictionaryReadCache::get(DictionaryId id)
  {
    if (_entriesPtr != nullptr && id != kInvalidDictionaryId)
    {
      if (auto const& entry = (*_entriesPtr)[id.raw() % kCapacity]; entry.id == id)
      {
        return entry.value;
      }
    }

    auto const value = _dictionary->get(id);

    if (!value.empty())
    {
      if (_entriesPtr == nullptr)
      {
        _entriesPtr = std::make_unique<std::array<Entry, kCapacity>>();
      }

      (*_entriesPtr)[id.raw() % kCapacity] = Entry{.id = id, .value = value};
    }

    return value;
  }

  DictionaryStore const& DictionaryReadCache::dictionary() const noexcept
  {
    return *_dictionary;
  }
} // namespace ao::library
