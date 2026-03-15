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

  void const* Dictionary::store(lmdb::WriteTransaction& txn, DictionaryId id, std::string_view value)
  {
    auto writer = _db.writer(txn);
    auto* ptr = writer.update(id.value(), boost::asio::buffer(value.data(), value.size()));
    if (ptr != nullptr)
    {
      _stringToId[std::string(value)] = id;
    }
    return ptr;
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
    auto it = _stringToId.find(std::string(str));
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
    return _stringToId.contains(std::string(str));
  }

} // namespace rs::core
