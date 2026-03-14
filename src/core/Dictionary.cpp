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

#include <rs/core/Dictionary.h>

namespace rs::core
{

  Dictionary::Dictionary(lmdb::MDB readDb, lmdb::MDB writeDb, DictionaryId nextId)
    : _readDb{readDb}
    , _writeDb{writeDb}
    , _nextId{nextId}
  {
  }

  DictionaryId Dictionary::getId(lmdb::Transaction& txn, std::string_view value)
  {
    if (value.empty())
    {
      return DictionaryId{0};
    }

    // Check cache first
    auto cacheIt = _stringToIdCache.find(std::string(value));
    if (cacheIt != _stringToIdCache.end())
    {
      return cacheIt->second;
    }

    // Check write DB (string → ID)
    std::string_view existingIdBytes;
    if (_writeDb.get(txn, value, existingIdBytes))
    {
      auto id = DictionaryId{lmdb::read<std::uint32_t>(existingIdBytes)};
      _stringToIdCache[std::string(value)] = id;
      _idToStringCache[id] = std::string(value);
      return id;
    }

    // Create new ID
    auto id = _nextId++;
    _writeDb.put(txn, value, lmdb::bytesOf(id.value()));
    _readDb.put(txn, lmdb::bytesOf(id.value()), value);

    // Update cache
    _stringToIdCache[std::string(value)] = id;
    _idToStringCache[id] = std::string(value);

    return id;
  }

  std::string_view Dictionary::getString(lmdb::Transaction& txn, DictionaryId id) const
  {
    if (id.value() == 0)
    {
      return {};
    }

    // Check cache first
    auto cacheIt = _idToStringCache.find(id);
    if (cacheIt != _idToStringCache.end())
    {
      return cacheIt->second;
    }

    // Look up in read DB
    std::string_view result;
    if (_readDb.get(txn, lmdb::bytesOf(id.value()), result))
    {
      return result;
    }

    return {};
  }

  DictionaryId Dictionary::getStringId(std::string_view str) const
  {
    if (str.empty())
    {
      return DictionaryId{0};
    }

    // Check cache first (most efficient)
    auto cacheIt = _stringToIdCache.find(std::string(str));
    if (cacheIt != _stringToIdCache.end())
    {
      return cacheIt->second;
    }

    // Return 0 if not found - caller can decide how to handle unknown strings
    return DictionaryId{0};
  }

} // namespace rs::core
