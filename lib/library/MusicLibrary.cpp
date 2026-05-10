// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/MusicLibrary.h>

#include <ao/library/DictionaryStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetaStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>

#include <ao/Exception.h>

#include <algorithm>
#include <chrono>
#include <random>

namespace
{
  // LMDB configuration constants
  constexpr std::size_t kLmdbMapSize = std::size_t{1} * 1024 * 1024 * 1024; // 1 GB
  constexpr std::uint32_t kLmdbMaxDatabases =
    8; // tracks_hot, tracks_cold, lists, resources, dictionary, meta (+ spare)
  constexpr std::uint32_t kLmdbFileMode = 0664;
  constexpr std::size_t kLibraryIdBytes = 16;

  std::uint64_t nowUnixMs()
  {
    auto const now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(now.time_since_epoch().count());
  }

  std::array<std::byte, kLibraryIdBytes> generateLibraryId()
  {
    auto bytes = std::array<std::byte, kLibraryIdBytes>{};
    auto random = std::random_device{};
    std::ranges::generate(bytes, [&random] { return static_cast<std::byte>(random()); });
    return bytes;
  }

  ao::library::MetaHeader makeMetaHeader()
  {
    auto const timestamp = nowUnixMs();
    return ao::library::MetaHeader{.magic = ao::library::kLibraryMetaMagic,
                                   .libraryVersion = ao::library::kLibraryVersion,
                                   .flags = 0,
                                   .createdAtUnixMs = timestamp,
                                   .migratedAtUnixMs = timestamp,
                                   .libraryId = generateLibraryId()};
  }

  void validateMetaHeader(ao::library::MetaHeader const& header)
  {
    if (header.magic != ao::library::kLibraryMetaMagic)
    {
      AO_THROW_FORMAT(ao::Exception,
                      "Invalid library metadata magic 0x{:08x} (expected 0x{:08x})",
                      header.magic,
                      ao::library::kLibraryMetaMagic);
    }

    if (header.libraryVersion > ao::library::kLibraryVersion)
    {
      AO_THROW_FORMAT(ao::Exception,
                      "Unsupported library version {} (maximum supported {})",
                      header.libraryVersion,
                      ao::library::kLibraryVersion);
    }

    if (header.libraryVersion < ao::library::kLibraryVersion)
    {
      AO_THROW_FORMAT(ao::Exception,
                      "Library version {} requires migration to version {}",
                      header.libraryVersion,
                      ao::library::kLibraryVersion);
    }
  }
}

namespace ao::library
{
  struct MusicLibrary::Impl final
  {
    std::filesystem::path const root;
    ao::lmdb::Environment env;
    ao::lmdb::WriteTransaction setupTxn;
    MetaStore metaStore;
    MetaHeader metaHeader{};
    TrackStore tracks;
    ListStore lists;
    ResourceStore resources;
    DictionaryStore dictionary;

    explicit Impl(std::filesystem::path rootPath)
      : root{std::move(rootPath)}
      , env{root.string(),
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
    {
    }
  };

  MusicLibrary::MusicLibrary(std::filesystem::path rootPath)
    : _impl{std::make_unique<Impl>(std::move(rootPath))}
  {
    if (auto const header = _impl->metaStore.load(_impl->setupTxn))
    {
      validateMetaHeader(*header);
      _impl->metaHeader = *header;
    }
    else
    {
      _impl->metaHeader = makeMetaHeader();
      _impl->metaStore.create(_impl->setupTxn, _impl->metaHeader);
    }

    // Load dictionary entries before first commit
    _impl->setupTxn.commit();
  }

  MusicLibrary::~MusicLibrary() = default;

  ao::lmdb::ReadTransaction MusicLibrary::readTransaction() const
  {
    return ao::lmdb::ReadTransaction{_impl->env};
  }

  ao::lmdb::WriteTransaction MusicLibrary::writeTransaction()
  {
    return ao::lmdb::WriteTransaction{_impl->env};
  }

  TrackStore& MusicLibrary::tracks()
  {
    return _impl->tracks;
  }

  TrackStore const& MusicLibrary::tracks() const
  {
    return _impl->tracks;
  }

  ListStore& MusicLibrary::lists()
  {
    return _impl->lists;
  }

  ListStore const& MusicLibrary::lists() const
  {
    return _impl->lists;
  }

  ResourceStore& MusicLibrary::resources()
  {
    return _impl->resources;
  }

  ResourceStore const& MusicLibrary::resources() const
  {
    return _impl->resources;
  }

  DictionaryStore& MusicLibrary::dictionary()
  {
    return _impl->dictionary;
  }

  DictionaryStore const& MusicLibrary::dictionary() const
  {
    return _impl->dictionary;
  }

  MetaHeader const& MusicLibrary::metaHeader() const
  {
    return _impl->metaHeader;
  }

  std::filesystem::path const& MusicLibrary::rootPath() const
  {
    return _impl->root;
  }
}
