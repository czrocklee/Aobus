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

#include <rs/core/MusicLibrary.h>

namespace
{
  // LMDB configuration constants
  constexpr std::size_t kLmdbMapSize = 1 * 1024 * 1024 * 1024; // 1 GB
  constexpr int kLmdbMaxDatabases = 5;                         // tracks, lists, resources, dict_read, dict_write
  constexpr int kLmdbFileMode = 0664;

  rs::lmdb::Environment createEnv(char const* path)
  {
    auto env = rs::lmdb::Environment::create();
    env.setMapSize(kLmdbMapSize);
    env.setMaxDatabases(kLmdbMaxDatabases);
    env.open(path, MDB_NOTLS, kLmdbFileMode);
    return env;
  }
}

namespace rs::core
{
  MusicLibrary::MusicLibrary(std::filesystem::path rootPath)
    : _root{std::move(rootPath)}
    , _env{createEnv(_root.c_str())}
    , _tracks{_env, "tracks"}
    , _lists{_env, "lists"}
    , _resources{_env, "resources"}
    , _dictReadDb{_env, "dict_read"}
    , _dictWriteDb{_env, "dict_write"}
  {
    // Find the highest existing ID to determine next available ID
    DictionaryId nextId{0};
    rs::lmdb::ReadTransaction readTxn{_env};
    auto reader = _dictReadDb.reader(readTxn);
    for (auto const& [id, value] : reader)
    {
      (void)value; // We only care about the key (id)
      if (static_cast<std::uint32_t>(id) >= nextId.value())
      {
        nextId = DictionaryId{static_cast<std::uint32_t>(id) + 1};
      }
    }

    _dictionary = std::make_unique<Dictionary>(_dictReadDb, _dictWriteDb, nextId);
  }
}
