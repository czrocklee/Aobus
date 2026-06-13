// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Exception.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/Meta.h>
#include <ao/library/MetaStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <lmdb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <utility>

namespace ao::library
{
  namespace
  {
    // LMDB configuration constants
    constexpr std::size_t kLmdbMapSize = std::size_t{1} * 1024 * 1024 * 1024; // 1 GB
    constexpr std::uint32_t kLmdbMaxDatabases =
      8; // tracks_hot, tracks_cold, lists, resources, dictionary, meta (+ spare)
    constexpr std::uint32_t kLmdbFileMode = 0664;
    constexpr std::size_t kLibraryIdBytes = 16;

    std::chrono::sys_time<std::chrono::milliseconds> currentTimestamp()
    {
      return std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    }

    std::array<std::byte, kLibraryIdBytes> generateLibraryId()
    {
      auto bytes = std::array<std::byte, kLibraryIdBytes>{};
      auto random = std::random_device{};
      std::ranges::generate(bytes, [&random] { return static_cast<std::byte>(random()); });
      return bytes;
    }

    MetaHeader makeMetaHeader()
    {
      auto const timestamp = currentTimestamp();
      return MetaHeader{.magic = kLibraryMetaMagic,
                        .libraryVersion = kLibraryVersion,
                        .flags = 0,
                        .createdTime = timestamp,
                        .libraryId = generateLibraryId()};
    }

    void validateMetaHeader(MetaHeader const& header)
    {
      if (header.magic != kLibraryMetaMagic)
      {
        throwException<Exception>(
          "Invalid library metadata magic 0x{:08x} (expected 0x{:08x})", header.magic, kLibraryMetaMagic);
      }

      if (header.libraryVersion > kLibraryVersion)
      {
        throwException<Exception>(
          "Unsupported library version {} (maximum supported {})", header.libraryVersion, kLibraryVersion);
      }
    }
  }

  struct MusicLibrary::Impl final
  {
    std::filesystem::path const musicRoot;
    std::filesystem::path const databasePath;
    lmdb::Environment env;
    lmdb::WriteTransaction setupTxn;
    MetaStore metaStore;
    MetaHeader metaHeader{};
    TrackStore tracks;
    ListStore lists;
    ResourceStore resources;
    DictionaryStore dictionary;
    FileManifestStore manifest;

    explicit Impl(std::filesystem::path musicRoot, std::filesystem::path databasePath)
      : musicRoot{std::move(musicRoot)}
      , databasePath{std::move(databasePath)}
      , env{this->databasePath.string(),
            lmdb::Environment::Options{.flags = MDB_NOTLS,
                                       .mode = kLmdbFileMode,
                                       .maxDatabases = kLmdbMaxDatabases,
                                       .mapSize = kLmdbMapSize}}
      , setupTxn{env}
      , metaStore{lmdb::Database{setupTxn, "meta"}}
      , tracks{lmdb::Database{setupTxn, "tracks_hot"}, lmdb::Database{setupTxn, "tracks_cold"}}
      , lists{lmdb::Database{setupTxn, "lists"}}
      , resources{lmdb::Database{setupTxn, "resources"}}
      , dictionary{lmdb::Database{setupTxn, "dictionary"}, setupTxn}
      , manifest{lmdb::Database{setupTxn, "file_manifest", lmdb::Database::KeyKind::Blob}}
    {
    }
  };

  MusicLibrary::MusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath)
  {
    std::filesystem::create_directories(databasePath);
    _implPtr = std::make_unique<Impl>(std::move(musicRoot), std::move(databasePath));

    if (auto const optHeader = _implPtr->metaStore.load(_implPtr->setupTxn); optHeader)
    {
      validateMetaHeader(*optHeader);
      _implPtr->metaHeader = *optHeader;
    }
    else
    {
      _implPtr->metaHeader = makeMetaHeader();
      _implPtr->metaStore.create(_implPtr->setupTxn, _implPtr->metaHeader);
    }

    // Load dictionary entries before first commit
    _implPtr->setupTxn.commit();
  }

  MusicLibrary::~MusicLibrary() = default;

  lmdb::ReadTransaction MusicLibrary::readTransaction() const
  {
    return lmdb::ReadTransaction{_implPtr->env};
  }

  lmdb::WriteTransaction MusicLibrary::writeTransaction()
  {
    return lmdb::WriteTransaction{_implPtr->env};
  }

  TrackStore& MusicLibrary::tracks()
  {
    return _implPtr->tracks;
  }

  TrackStore const& MusicLibrary::tracks() const
  {
    return _implPtr->tracks;
  }

  ListStore& MusicLibrary::lists()
  {
    return _implPtr->lists;
  }

  ListStore const& MusicLibrary::lists() const
  {
    return _implPtr->lists;
  }

  ResourceStore& MusicLibrary::resources()
  {
    return _implPtr->resources;
  }

  ResourceStore const& MusicLibrary::resources() const
  {
    return _implPtr->resources;
  }

  DictionaryStore& MusicLibrary::dictionary()
  {
    return _implPtr->dictionary;
  }

  DictionaryStore const& MusicLibrary::dictionary() const
  {
    return _implPtr->dictionary;
  }

  FileManifestStore& MusicLibrary::manifest()
  {
    return _implPtr->manifest;
  }

  FileManifestStore const& MusicLibrary::manifest() const
  {
    return _implPtr->manifest;
  }

  MetaHeader const& MusicLibrary::metaHeader() const
  {
    return _implPtr->metaHeader;
  }

  void MusicLibrary::updateMetaHeader(MetaHeader const& header)
  {
    validateMetaHeader(header);
    auto txn = writeTransaction();
    _implPtr->metaStore.update(txn, header);
    txn.commit();
    _implPtr->metaHeader = header;
  }

  std::filesystem::path const& MusicLibrary::rootPath() const
  {
    return _implPtr->musicRoot;
  }
}
