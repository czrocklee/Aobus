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

  Dictionary::Dictionary(lmdb::Database& db) : _db{db}
  {
  }

  DictionaryId Dictionary::put(lmdb::WriteTransaction& txn, std::string_view value)
  {
    // Check in-memory index first (transparent lookup - no string creation)
    auto it = _stringToId.find(value);
    if (it != _stringToId.end())
    {
      return it->second;
    }

    // Check database for existing entry
    auto reader = _db.reader(txn);
    for (auto&& [id, buf] : reader)
    {
      auto size = boost::asio::buffer_size(buf);
      if (size > 0)
      {
        auto existing = std::string_view(static_cast<char const*>(buf.data()), size);
        if (existing == value)
        {
          auto dictId = static_cast<std::uint32_t>(id);
          _stringToId[std::string(value)] = DictionaryId{dictId};
          return DictionaryId{dictId};
        }
      }
    }

    // Not found - append new entry
    auto writer = _db.writer(txn);
    auto [id, buffer] = writer.append(boost::asio::buffer(value.data(), value.size()));
    auto dictId = static_cast<std::uint32_t>(id);
    if (buffer != nullptr)
    {
      _stringToId[std::string(value)] = DictionaryId{dictId};
    }
    return DictionaryId{dictId};
  }

  std::string_view Dictionary::get(lmdb::ReadTransaction& txn, DictionaryId id) const
  {
    auto reader = _db.reader(txn);
    auto value = reader[id.value()];
    auto size = boost::asio::buffer_size(value);
    if (size > 0)
    {
      return {static_cast<char const*>(value.data()), size};
    }
    return {};
  }

  DictionaryId Dictionary::getId(std::string_view str) const
  {
    if (str.empty())
    {
      return DictionaryId{0};
    }
    auto it = _stringToId.find(str);
    if (it != _stringToId.end())
    {
      return it->second;
    }
    return DictionaryId{0};
  }

  bool Dictionary::contains(lmdb::ReadTransaction& txn, DictionaryId id) const
  {
    auto reader = _db.reader(txn);
    auto value = reader[id.value()];
    return boost::asio::buffer_size(value) > 0;
  }

  bool Dictionary::contains(std::string_view str) const
  {
    return _stringToId.contains(str);
  }

} // namespace rs::core
