/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <rs/ml/core/ResourceStore.h>
#include <boost/uuid/detail/md5.hpp>
#include <iostream>

namespace
{
  std::uint64_t hash(boost::asio::const_buffer buffer)
  {
    using boost::uuids::detail::md5;
    md5 hash;
    md5::digest_type digest;
    hash.process_bytes(buffer.data(), buffer.size());
    hash.get_digest(digest);
    return *reinterpret_cast<std::uint64_t*>(digest);
  }
}

namespace rs::ml::core
{
  using Writer = ResourceStore::Writer;

  Writer ResourceStore::writer(LMDBWriteTransaction& txn)
  {
    return Writer{_database.reader(txn), _database.writer(txn)};
  }

  std::uint64_t Writer::create(boost::asio::const_buffer buffer)
  {
    for (std::uint64_t key = hash(buffer);; ++key) // linear probe
    {
      boost::asio::const_buffer value = _reader[key];

      if (value.data() == nullptr)
      {
        void* data = _writer.create(key, buffer.size());
        std::cout << "write " << buffer.size() << std::endl;
        std::memcpy(data, buffer.data(), buffer.size());
        return key;
      }

      if (value.size() == buffer.size() && std::memcmp(value.data(), buffer.data(), buffer.size()) == 0)
      {
        return key;
      }
    }
  }
}
