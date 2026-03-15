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

#pragma once

#include <lmdb.h>

#include <cstddef>
#include <memory>
#include <string_view>

namespace rs::lmdb
{
  class Environment
  {
  public:
    static constexpr unsigned int DefaultFlags = 0;
    static constexpr mdb_mode_t DefaultMode = 0644;

    Environment();

    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;

    Environment(Environment&& other) noexcept;
    Environment& operator=(Environment&& other) noexcept;

    ~Environment() noexcept;

    Environment& open(std::string const& path, unsigned int flags = DefaultFlags, mdb_mode_t mode = DefaultMode);
    Environment& setMapSize(std::size_t size);
    Environment& setMaxDatabases(MDB_dbi count);
    Environment& setMaxReaders(unsigned int count);
    void close() noexcept;

  private:
    struct EnvDeleter { void operator()(MDB_env* env) const { mdb_env_close(env); } };
    std::unique_ptr<MDB_env, EnvDeleter> _handle;
    
    friend class Database;
    friend class ReadTransaction;
    friend class WriteTransaction;
  };
}
