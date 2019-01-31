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

#include <rs/ml/core/MusicLibrary.h>

namespace
{
  lmdb::env createEnv(const std::string& rootDir)
  {
    auto env = lmdb::env::create();
    env.set_mapsize(1UL * 1024UL * 1024UL * 1024UL);
    env.set_max_dbs(2);
    env.open(rootDir.c_str(), MDB_NOTLS, 0664);
    return env;
  }
}

namespace rs::ml::core
{
  MusicLibrary::MusicLibrary(const std::string& rootDir)
    : _env{createEnv(rootDir)},
      _tracks{_env, "tracks"},
      _lists{_env, "lists"}
  {
  }
}

