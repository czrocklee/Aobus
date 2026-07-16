// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "LibraryIdentity.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WriteTransaction.h>
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
#include <expected>
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

    MetadataHeader makeMetadataHeader()
    {
      auto const timestamp = currentTimestamp();
      return MetadataHeader{.magic = kMetadataMagic,
                            .libraryVersion = kLibraryVersion,
                            .flags = 0,
                            .createdTime = timestamp,
                            .libraryId = generateLibraryId()};
    }

    Result<> validateMetadataHeader(MetadataHeader const& header)
    {
      if (header.magic != kMetadataMagic)
      {
        return makeError(
          Error::Code::CorruptData,
          std::format("Invalid library metadata magic 0x{:08x} (expected 0x{:08x})", header.magic, kMetadataMagic));
      }

      if (header.libraryVersion != kLibraryVersion)
      {
        return makeError(
          Error::Code::CorruptData,
          std::format("Unsupported library version {} (current {})", header.libraryVersion, kLibraryVersion));
      }

      return {};
    }
  } // namespace

  struct MusicLibrary::Impl final
  {
    std::filesystem::path const musicRoot;
    std::filesystem::path const databasePath;
    lmdb::Environment env;
    lmdb::WriteTransaction initializationTransaction;
    detail::LibraryIdentity identity;
    MetadataStore metadataStore;
    TrackStore tracks;
    ListStore lists;
    ResourceStore resources;
    DictionaryStore dictionary;
    FileManifestStore manifest;

    Impl(std::filesystem::path musicRoot,
         std::filesystem::path databasePath,
         lmdb::Environment env,
         lmdb::WriteTransaction initializationTransaction,
         lmdb::Database metadataDb,
         lmdb::Database tracksHotDb,
         lmdb::Database tracksColdDb,
         lmdb::Database listsDb,
         lmdb::Database resourcesDb,
         lmdb::Database dictionaryDb,
         lmdb::Database manifestDb)
      : musicRoot{std::move(musicRoot)}
      , databasePath{std::move(databasePath)}
      , env{std::move(env)}
      , initializationTransaction{std::move(initializationTransaction)}
      , metadataStore{std::move(metadataDb), identity}
      , tracks{std::move(tracksHotDb), std::move(tracksColdDb), identity}
      , lists{std::move(listsDb), identity}
      , resources{std::move(resourcesDb), identity}
      , dictionary{std::move(dictionaryDb), this->initializationTransaction, identity}
      , manifest{std::move(manifestDb), identity}
    {
    }

    static Result<std::unique_ptr<Impl>> create(std::filesystem::path musicRoot,
                                                std::filesystem::path databasePath,
                                                std::size_t mapSize)
    {
      if (mapSize == 0)
      {
        mapSize = kLmdbMapSize;
      }

      auto env = lmdb::Environment::open(
        databasePath.string(),
        lmdb::Environment::Options{
          .flags = lmdb::kEnvNoTls, .mode = kLmdbFileMode, .maxDatabases = kLmdbMaxDatabases, .mapSize = mapSize});

      if (!env)
      {
        return std::unexpected{env.error()};
      }

      auto initializationTransaction = lmdb::WriteTransaction::begin(*env);

      if (!initializationTransaction)
      {
        return std::unexpected{initializationTransaction.error()};
      }

      auto metadataDb = lmdb::Database::open(*initializationTransaction, "meta");
      auto tracksHotDb = lmdb::Database::open(*initializationTransaction, "tracks_hot");
      auto tracksColdDb = lmdb::Database::open(*initializationTransaction, "tracks_cold");
      auto listsDb = lmdb::Database::open(*initializationTransaction, "lists");
      auto resourcesDb = lmdb::Database::open(*initializationTransaction, "resources");
      auto dictionaryDb = lmdb::Database::open(*initializationTransaction, "dictionary");
      auto manifestDb =
        lmdb::Database::open(*initializationTransaction, "file_manifest", lmdb::Database::KeyKind::Blob);

      if (!metadataDb)
      {
        return std::unexpected{metadataDb.error()};
      }

      if (!tracksHotDb)
      {
        return std::unexpected{tracksHotDb.error()};
      }

      if (!tracksColdDb)
      {
        return std::unexpected{tracksColdDb.error()};
      }

      if (!listsDb)
      {
        return std::unexpected{listsDb.error()};
      }

      if (!resourcesDb)
      {
        return std::unexpected{resourcesDb.error()};
      }

      if (!dictionaryDb)
      {
        return std::unexpected{dictionaryDb.error()};
      }

      if (!manifestDb)
      {
        return std::unexpected{manifestDb.error()};
      }

      return std::make_unique<Impl>(std::move(musicRoot),
                                    std::move(databasePath),
                                    std::move(*env),
                                    std::move(*initializationTransaction),
                                    std::move(*metadataDb),
                                    std::move(*tracksHotDb),
                                    std::move(*tracksColdDb),
                                    std::move(*listsDb),
                                    std::move(*resourcesDb),
                                    std::move(*dictionaryDb),
                                    std::move(*manifestDb));
    }
  };

  MusicLibrary::MusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath)
    : MusicLibrary{std::move(musicRoot), std::move(databasePath), Options{}}
  {
  }

  MusicLibrary::MusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath, Options options)
  {
    if (auto result = initialize(std::move(musicRoot), std::move(databasePath), options); !result)
    {
      throwException<Exception>("Failed to open music library: {}", result.error().message);
    }
  }

  MusicLibrary::~MusicLibrary() = default;
  MusicLibrary::MusicLibrary(MusicLibrary&&) noexcept = default;
  MusicLibrary& MusicLibrary::operator=(MusicLibrary&&) noexcept = default;

  Result<MusicLibrary> MusicLibrary::open(std::filesystem::path musicRoot, std::filesystem::path databasePath)
  {
    return open(std::move(musicRoot), std::move(databasePath), Options{});
  }

  Result<MusicLibrary> MusicLibrary::open(std::filesystem::path musicRoot,
                                          std::filesystem::path databasePath,
                                          Options options)
  {
    auto library = MusicLibrary{};

    if (auto result = library.initialize(std::move(musicRoot), std::move(databasePath), options); !result)
    {
      return std::unexpected{result.error()};
    }

    return library;
  }

  Result<> MusicLibrary::initialize(std::filesystem::path musicRoot,
                                    std::filesystem::path databasePath,
                                    Options options)
  {
    try
    {
      std::filesystem::create_directories(databasePath);
      auto impl = Impl::create(std::move(musicRoot), std::move(databasePath), options.mapSize);

      if (!impl)
      {
        return std::unexpected{impl.error()};
      }

      _implPtr = std::move(*impl);

      auto headerResult = _implPtr->metadataStore.load(_implPtr->initializationTransaction);

      if (!headerResult && headerResult.error().code != Error::Code::NotFound)
      {
        return std::unexpected{headerResult.error()};
      }

      if (headerResult)
      {
        if (auto result = validateMetadataHeader(*headerResult); !result)
        {
          return std::unexpected{result.error()};
        }
      }
      else
      {
        auto const header = makeMetadataHeader();

        if (auto result = _implPtr->metadataStore.create(_implPtr->initializationTransaction, header); !result)
        {
          return result;
        }
      }

      if (auto result = _implPtr->initializationTransaction.commit(); !result)
      {
        return std::unexpected{result.error()};
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

  ReadTransaction MusicLibrary::readTransaction() const
  {
    auto transaction = lmdb::ReadTransaction::begin(_implPtr->env);

    if (!transaction)
    {
      throwException<Exception>("Failed to begin read transaction: {}", transaction.error().message);
    }

    return ReadTransaction{std::move(*transaction), _implPtr->identity};
  }

  WriteTransaction MusicLibrary::beginWriteTransaction(WriteTransaction::Options options,
                                                       std::shared_ptr<void const> writerSessionAnchorPtr)
  {
    auto transaction = WriteTransaction::begin(
      _implPtr->env, _implPtr->dictionary, _implPtr->identity, std::move(options), std::move(writerSessionAnchorPtr));

    if (!transaction)
    {
      throwException<Exception>("Failed to begin write transaction: {}", transaction.error().message);
    }

    _implPtr->metadataStore.bumpRevision(transaction->native(_implPtr->identity));
    return std::move(*transaction);
  }

  std::uint64_t MusicLibrary::libraryRevision(ReadTransaction const& transaction) const
  {
    return _implPtr->metadataStore.revision(transaction);
  }

  std::uint64_t MusicLibrary::libraryRevision(WriteTransaction const& transaction) const
  {
    return _implPtr->metadataStore.revision(transaction);
  }

  TrackStore const& MusicLibrary::tracks() const
  {
    return _implPtr->tracks;
  }

  ListStore const& MusicLibrary::lists() const
  {
    return _implPtr->lists;
  }

  ResourceStore const& MusicLibrary::resources() const
  {
    return _implPtr->resources;
  }

  DictionaryStore const& MusicLibrary::dictionary() const
  {
    return _implPtr->dictionary;
  }

  FileManifestStore const& MusicLibrary::manifest() const
  {
    return _implPtr->manifest;
  }

  MetadataStore const& MusicLibrary::metadata() const
  {
    return _implPtr->metadataStore;
  }

  MetadataHeader MusicLibrary::metadataHeader() const
  {
    auto transaction = readTransaction();
    auto header = _implPtr->metadataStore.load(transaction);

    if (!header)
    {
      throwException<Exception>("Failed to load library metadata header: {}", header.error().message);
    }

    return *header;
  }

  std::filesystem::path const& MusicLibrary::rootPath() const
  {
    return _implPtr->musicRoot;
  }

  std::filesystem::path const& MusicLibrary::databasePath() const
  {
    return _implPtr->databasePath;
  }
} // namespace ao::library
