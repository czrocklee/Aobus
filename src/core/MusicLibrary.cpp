// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/MusicLibrary.h>

#include <rs/Exception.h>
#include <rs/core/ListLayout.h>
#include <rs/core/TrackLayout.h>

#include <algorithm>
#include <chrono>
#include <random>

namespace
{
  // LMDB configuration constants
  constexpr std::size_t kLmdbMapSize = std::size_t{1} * 1024 * 1024 * 1024; // 1 GB
  constexpr int kLmdbMaxDatabases = 8; // tracks_hot, tracks_cold, lists, resources, dictionary, meta (+ spare)
  constexpr int kLmdbFileMode = 0664;

  std::uint64_t nowUnixMs()
  {
    auto const now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(now.time_since_epoch().count());
  }

  std::array<std::byte, 16> generateLibraryId()
  {
    auto bytes = std::array<std::byte, 16>{};
    auto random = std::random_device{};
    std::generate(bytes.begin(), bytes.end(), [&random]() { return static_cast<std::byte>(random()); });
    return bytes;
  }

  rs::core::LibraryMetaHeader makeMetaHeader()
  {
    auto const timestamp = nowUnixMs();
    return rs::core::LibraryMetaHeader{.magic = rs::core::kLibraryMetaMagic,
                                       .headerVersion = rs::core::kLibraryMetaHeaderVersion,
                                       .librarySchemaVersion = rs::core::kLibrarySchemaVersion,
                                       .trackLayoutVersion = rs::core::kTrackLayoutVersion,
                                       .listLayoutVersion = rs::core::kListLayoutVersion,
                                       .flags = 0,
                                       .createdAtUnixMs = timestamp,
                                       .migratedAtUnixMs = timestamp,
                                       .libraryId = generateLibraryId()};
  }

  void validateMetaHeader(rs::core::LibraryMetaHeader const& header)
  {
    if (header.magic != rs::core::kLibraryMetaMagic)
    {
      RS_THROW_FORMAT(rs::Exception,
                      "Invalid library metadata magic 0x{:08x} (expected 0x{:08x})",
                      header.magic,
                      rs::core::kLibraryMetaMagic);
    }

    if (header.headerVersion != rs::core::kLibraryMetaHeaderVersion)
    {
      RS_THROW_FORMAT(rs::Exception,
                      "Unsupported library metadata header version {} (expected {})",
                      header.headerVersion,
                      rs::core::kLibraryMetaHeaderVersion);
    }

    if (header.librarySchemaVersion != rs::core::kLibrarySchemaVersion)
    {
      RS_THROW_FORMAT(rs::Exception,
                      "Unsupported library schema version {} (expected {})",
                      header.librarySchemaVersion,
                      rs::core::kLibrarySchemaVersion);
    }

    if (header.trackLayoutVersion != rs::core::kTrackLayoutVersion)
    {
      RS_THROW_FORMAT(rs::Exception,
                      "Unsupported track layout version {} (expected {})",
                      header.trackLayoutVersion,
                      rs::core::kTrackLayoutVersion);
    }

    if (header.listLayoutVersion != rs::core::kListLayoutVersion)
    {
      RS_THROW_FORMAT(rs::Exception,
                      "Unsupported list layout version {} (expected {})",
                      header.listLayoutVersion,
                      rs::core::kListLayoutVersion);
    }
  }
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
    , _metaStore{lmdb::Database{_txn, "meta"}}
    , _tracks{lmdb::Database{_txn, "tracks_hot"}, lmdb::Database{_txn, "tracks_cold"}}
    , _lists{lmdb::Database{_txn, "lists"}}
    , _resources{lmdb::Database{_txn, "resources"}}
    , _dictionary{lmdb::Database{_txn, "dictionary"}, _txn}
  {
    if (auto const header = _metaStore.load(_txn))
    {
      validateMetaHeader(*header);
      _metaHeader = *header;
    }
    else
    {
      _metaHeader = makeMetaHeader();
      _metaStore.create(_txn, _metaHeader);
    }

    // Load dictionary entries before first commit
    _txn.commit();
  }
}
