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
  rs::lmdb::Environment createEnv(const char* path)
  {
    auto env = rs::lmdb::Environment::create();
    env.setMapSize(1UL * 1024UL * 1024UL * 1024UL);
    env.setMaxDatabases(5); // tracks, lists, resources, dict_read, dict_write
    env.open(path, MDB_NOTLS, 0664);
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
  {
    // Open dictionary databases within a write transaction
    rs::lmdb::WriteTransaction writeTxn{_env};
    _dictReadDb = lmdb::MDB::open(writeTxn.transaction(), "dict_read", MDB_CREATE);
    _dictWriteDb = lmdb::MDB::open(writeTxn.transaction(), "dict_write", MDB_CREATE);

    // Find the highest existing ID to determine next available ID
    DictionaryId nextId{0};
    std::string_view key;
    std::string_view value;
    auto cursor = lmdb::Cursor::open(writeTxn.transaction().raw(), _dictReadDb.raw());
    if (cursor.get(key, value, MDB_FIRST))
    {
      do
      {
        if (key.size() == sizeof(std::uint32_t))
        {
          auto id = DictionaryId{lmdb::read<std::uint32_t>(key)};
          if (id.value() >= nextId.value())
          {
            nextId = DictionaryId{id.value() + 1};
          }
        }
      }
      while (cursor.get(key, value, MDB_NEXT));
    }

    _dictionary = std::make_unique<Dictionary>(_dictReadDb, _dictWriteDb, nextId);
    writeTxn.commit();
  }
}
