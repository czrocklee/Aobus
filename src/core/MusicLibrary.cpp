// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/MusicLibrary.h>

namespace
{
  // LMDB configuration constants
  constexpr std::size_t kLmdbMapSize = std::size_t{1} * 1024 * 1024 * 1024; // 1 GB
  constexpr int kLmdbMaxDatabases = 6;                         // tracks, tracks_hot, tracks_cold, lists, resources, dictionary
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
    , _tracks{lmdb::Database{_txn, "tracks_hot"}, lmdb::Database{_txn, "tracks_cold"}}
    , _lists{lmdb::Database{_txn, "lists"}}
    , _resources{lmdb::Database{_txn, "resources"}}
    , _dictionary{lmdb::Database{_txn, "dictionary"}, _txn}
  {
    // Load dictionary entries before first commit
    _txn.commit();
  }
}
