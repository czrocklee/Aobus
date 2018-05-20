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

#include <lmdbxx/lmdb++.h>
#include <stack>

namespace rs::ml::core::impl
{
  class ReaderImplCache
  {
  public:
    using Impl = std::pair<lmdb::txn, lmdb::cursor>;

    Impl get(lmdb::env& env, lmdb::dbi& dbi)
    {
      if (_cache.empty())
      {
        auto txn = lmdb::txn::begin(env, nullptr, MDB_RDONLY);
        auto cursor = lmdb::cursor::open(txn, dbi);
        return {std::move(txn), std::move(cursor)};
      }
      
      Impl impl{std::move(_cache.top())};
      _cache.pop();
      impl.first.renew();
      impl.second.renew(impl.first);
      return std::move(impl);
    }

    void put(Impl&& impl)
    {
      if (_cache.size() < MaxCacheSize)
      {
        impl.first.reset();
        _cache.emplace(std::move(impl));
      }
    }

  private:
    constexpr static std::size_t MaxCacheSize = 256;
    std::stack<Impl> _cache;
  };
}
