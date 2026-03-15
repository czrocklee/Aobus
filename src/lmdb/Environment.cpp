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

#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Type.h>

namespace rs::lmdb
{
  Environment::Environment()
  {
    MDB_env* handle = nullptr;
    throwOnError("mdb_env_create", mdb_env_create(&handle));
    _handle.reset(handle);
  }

  Environment::Environment(Environment&& other) noexcept = default;

  Environment& Environment::operator=(Environment&& other) noexcept = default;

  Environment::~Environment() noexcept = default;

  Environment& Environment::open(std::string const& path, unsigned int flags, mdb_mode_t mode)
  {
    throwOnError("mdb_env_open", mdb_env_open(_handle.get(), path.c_str(), flags, mode));
    return *this;
  }

  Environment& Environment::setMapSize(std::size_t size)
  {
    throwOnError("mdb_env_set_mapsize", mdb_env_set_mapsize(_handle.get(), size));
    return *this;
  }

  Environment& Environment::setMaxDatabases(MDB_dbi count)
  {
    throwOnError("mdb_env_set_maxdbs", mdb_env_set_maxdbs(_handle.get(), count));
    return *this;
  }

  Environment& Environment::setMaxReaders(unsigned int count)
  {
    throwOnError("mdb_env_set_maxreaders", mdb_env_set_maxreaders(_handle.get(), count));
    return *this;
  }

  void Environment::close() noexcept { _handle.reset(); }
}
