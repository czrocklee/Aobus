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

#include <boost/asio/buffer.hpp>

namespace rs::core
{

  Dictionary::Dictionary(lmdb::Database& readDb, lmdb::Database& writeDb, DictionaryId nextId)
    : _readDb{readDb}
    , _writeDb{writeDb}
    , _nextId{nextId}
  {
  }

  lmdb::detail::MDB& Dictionary::writeDbRaw(lmdb::WriteTransaction& txn)
  {
    // We need to access the raw MDB for string-key operations
    // This is a workaround - in a proper design, Database would support string keys
    (void)txn; // txn is not used but kept for potential future use
    return _writeDb.raw();
  }

  DictionaryId Dictionary::getId(lmdb::WriteTransaction& txn, std::string_view value)
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

    // Use raw MDB access for string key operations in write DB
    auto& writeDb = writeDbRaw(txn);
    auto& transaction = txn.transaction();

    // Check write DB (string → ID)
    std::string_view existingIdBytes;
    if (writeDb.get(transaction, value, existingIdBytes))
    {
      auto id = DictionaryId{lmdb::read<std::uint32_t>(existingIdBytes)};
      _stringToIdCache[std::string(value)] = id;
      _idToStringCache[id] = std::string(value);
      return id;
    }

    // Create new ID
    auto id = _nextId++;

    // Store string → ID in write DB
    if (!writeDb.put(transaction, value, lmdb::bytesOf(id.value())))
    {
      throw std::runtime_error{"Failed to write to dictionary"};
    }

    // Store ID → string in read DB
    auto writer = _readDb.writer(txn);
    if (!writer.update(id.value(), boost::asio::buffer(value.data(), value.size())))
    {
      throw std::runtime_error{"Failed to write to read dictionary"};
    }

    // Update cache
    _stringToIdCache[std::string(value)] = id;
    _idToStringCache[id] = std::string(value);

    return id;
  }

  std::string_view Dictionary::getString(lmdb::ReadTransaction& txn, DictionaryId id) const
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

    // Look up in read DB using Database::Reader
    auto reader = _readDb.reader(txn);
    auto value = reader[id.value()];
    auto size = boost::asio::buffer_size(value);
    if (size > 0)
    {
      return {static_cast<char const*>(value.data()), size};
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
