// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
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

    Result<> validateMetaHeader(MetaHeader const& header)
    {
      if (header.magic != kLibraryMetaMagic)
      {
        return makeError(
          Error::Code::CorruptData,
          std::format("Invalid library metadata magic 0x{:08x} (expected 0x{:08x})", header.magic, kLibraryMetaMagic));
      }

      if (header.libraryVersion > kLibraryVersion)
      {
        return makeError(
          Error::Code::CorruptData,
          std::format("Unsupported library version {} (maximum supported {})", header.libraryVersion, kLibraryVersion));
      }

      return {};
    }
  } // namespace

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

    Impl(std::filesystem::path musicRoot,
         std::filesystem::path databasePath,
         lmdb::Environment env,
         lmdb::WriteTransaction setupTxn,
         lmdb::Database metaDb,
         lmdb::Database tracksHotDb,
         lmdb::Database tracksColdDb,
         lmdb::Database listsDb,
         lmdb::Database resourcesDb,
         lmdb::Database dictionaryDb,
         lmdb::Database manifestDb)
      : musicRoot{std::move(musicRoot)}
      , databasePath{std::move(databasePath)}
      , env{std::move(env)}
      , setupTxn{std::move(setupTxn)}
      , metaStore{std::move(metaDb)}
      , tracks{std::move(tracksHotDb), std::move(tracksColdDb)}
      , lists{std::move(listsDb)}
      , resources{std::move(resourcesDb)}
      , dictionary{std::move(dictionaryDb), this->setupTxn}
      , manifest{std::move(manifestDb)}
    {
    }

    static Result<std::unique_ptr<Impl>> create(std::filesystem::path musicRoot, std::filesystem::path databasePath)
    {
      auto env = lmdb::Environment::open(
        databasePath.string(),
        lmdb::Environment::Options{
          .flags = lmdb::kEnvNoTls, .mode = kLmdbFileMode, .maxDatabases = kLmdbMaxDatabases, .mapSize = kLmdbMapSize});

      if (!env)
      {
        return makeError(env.error().code, env.error().message);
      }

      auto setupTxn = lmdb::WriteTransaction::begin(*env);

      if (!setupTxn)
      {
        return makeError(setupTxn.error().code, setupTxn.error().message);
      }

      auto metaDb = lmdb::Database::open(*setupTxn, "meta");
      auto tracksHotDb = lmdb::Database::open(*setupTxn, "tracks_hot");
      auto tracksColdDb = lmdb::Database::open(*setupTxn, "tracks_cold");
      auto listsDb = lmdb::Database::open(*setupTxn, "lists");
      auto resourcesDb = lmdb::Database::open(*setupTxn, "resources");
      auto dictionaryDb = lmdb::Database::open(*setupTxn, "dictionary");
      auto manifestDb = lmdb::Database::open(*setupTxn, "file_manifest", lmdb::Database::KeyKind::Blob);

      if (!metaDb)
      {
        return makeError(metaDb.error().code, metaDb.error().message);
      }

      if (!tracksHotDb)
      {
        return makeError(tracksHotDb.error().code, tracksHotDb.error().message);
      }

      if (!tracksColdDb)
      {
        return makeError(tracksColdDb.error().code, tracksColdDb.error().message);
      }

      if (!listsDb)
      {
        return makeError(listsDb.error().code, listsDb.error().message);
      }

      if (!resourcesDb)
      {
        return makeError(resourcesDb.error().code, resourcesDb.error().message);
      }

      if (!dictionaryDb)
      {
        return makeError(dictionaryDb.error().code, dictionaryDb.error().message);
      }

      if (!manifestDb)
      {
        return makeError(manifestDb.error().code, manifestDb.error().message);
      }

      return std::make_unique<Impl>(std::move(musicRoot),
                                    std::move(databasePath),
                                    std::move(*env),
                                    std::move(*setupTxn),
                                    std::move(*metaDb),
                                    std::move(*tracksHotDb),
                                    std::move(*tracksColdDb),
                                    std::move(*listsDb),
                                    std::move(*resourcesDb),
                                    std::move(*dictionaryDb),
                                    std::move(*manifestDb));
    }
  };

  MusicLibrary::MusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath)
  {
    if (auto result = initialize(std::move(musicRoot), std::move(databasePath)); !result)
    {
      throwException<Exception>("Failed to open music library: {}", result.error().message);
    }
  }

  MusicLibrary::~MusicLibrary() = default;
  MusicLibrary::MusicLibrary(MusicLibrary&&) noexcept = default;
  MusicLibrary& MusicLibrary::operator=(MusicLibrary&&) noexcept = default;

  Result<MusicLibrary> MusicLibrary::open(std::filesystem::path musicRoot, std::filesystem::path databasePath)
  {
    auto library = MusicLibrary{};

    if (auto result = library.initialize(std::move(musicRoot), std::move(databasePath)); !result)
    {
      return makeError(result.error().code, result.error().message);
    }

    return library;
  }

  Result<> MusicLibrary::initialize(std::filesystem::path musicRoot, std::filesystem::path databasePath)
  {
    try
    {
      std::filesystem::create_directories(databasePath);
      auto impl = Impl::create(std::move(musicRoot), std::move(databasePath));

      if (!impl)
      {
        return makeError(impl.error().code, impl.error().message);
      }

      _implPtr = std::move(*impl);

      auto headerResult = _implPtr->metaStore.load(_implPtr->setupTxn);

      if (!headerResult && headerResult.error().code != Error::Code::NotFound)
      {
        return makeError(headerResult.error().code, headerResult.error().message);
      }

      if (headerResult)
      {
        if (auto result = validateMetaHeader(*headerResult); !result)
        {
          return makeError(result.error().code, result.error().message);
        }

        _implPtr->metaHeader = *headerResult;
      }
      else
      {
        _implPtr->metaHeader = makeMetaHeader();
        _implPtr->metaStore.create(_implPtr->setupTxn, _implPtr->metaHeader);
      }

      // Load dictionary entries before first commit
      if (auto result = _implPtr->setupTxn.commit(); !result)
      {
        return makeError(result.error().code, result.error().message);
      }

      return {};
    }
    catch (Exception const& ex)
    {
      return makeError(Error::Code::IoError, ex.what());
    }
    catch (std::filesystem::filesystem_error const& ex)
    {
      return makeError(Error::Code::IoError, ex.what());
    }
    catch (std::exception const& ex)
    {
      return makeError(Error::Code::IoError, ex.what());
    }
  }

  lmdb::ReadTransaction MusicLibrary::readTransaction() const
  {
    auto txn = lmdb::ReadTransaction::begin(_implPtr->env);

    if (!txn)
    {
      throwException<Exception>("Failed to begin read transaction: {}", txn.error().message);
    }

    return std::move(*txn);
  }

  lmdb::WriteTransaction MusicLibrary::writeTransaction()
  {
    auto txn = lmdb::WriteTransaction::begin(_implPtr->env);

    if (!txn)
    {
      throwException<Exception>("Failed to begin write transaction: {}", txn.error().message);
    }

    return std::move(*txn);
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
    if (auto result = validateMetaHeader(header); !result)
    {
      throwException<Exception>("Invalid library metadata header: {}", result.error().message);
    }

    auto txn = writeTransaction();
    _implPtr->metaStore.update(txn, header);

    if (auto result = txn.commit(); !result)
    {
      throwException<Exception>("Failed to update library metadata header: {}", result.error().message);
    }

    _implPtr->metaHeader = header;
  }

  std::filesystem::path const& MusicLibrary::rootPath() const
  {
    return _implPtr->musicRoot;
  }
} // namespace ao::library
