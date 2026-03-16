// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/MusicLibrary.h>

namespace
{
  // LMDB configuration constants
  constexpr std::size_t kLmdbMapSize = 1 * 1024 * 1024 * 1024; // 1 GB
  constexpr int kLmdbMaxDatabases = 4;                         // tracks, lists, resources, dictionary
  constexpr int kLmdbFileMode = 0664;
}

namespace rs::core
{
  MusicLibrary::MusicLibrary(std::filesystem::path rootPath)
    : _root{std::move(rootPath)}
    , _env{_root.string(),
           lmdb::Environment::Options{.flags = MDB_NOTLS,
                                      .mode = kLmdbFileMode,
                                      .maxDatabases = kLmdbMaxDatabases,
                                      .mapSize = kLmdbMapSize}}
    , _txn{_env}
    , _tracks{_txn, "tracks"}
    , _lists{_txn, "lists"}
    , _resources{_txn, "resources"}
    , _dictionary{_txn, "dictionary"}
  {
    // Load dictionary entries before first commit
    _txn.commit();
  }
}
