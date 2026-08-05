// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/MusicLibrary.h>

#include "FileManifestValidation.h"
#include "LibraryIdentity.h"
#include "TrackRecordValidation.h"
#include "detail/LibraryError.h"
#include "lmdb/detail/TransactionFailure.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/ExceptionFormat.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
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
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
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

    Result<std::array<std::byte, kLibraryIdBytes>> generateLibraryId()
    {
      try
      {
        auto bytes = std::array<std::byte, kLibraryIdBytes>{};
        auto random = std::random_device{};
        std::ranges::generate(bytes, [&random] { return static_cast<std::byte>(random()); });
        return bytes;
      }
      catch (std::runtime_error const& error)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to obtain library identity entropy: {}", error.what()));
      }
    }

    Result<MetadataHeader> makeMetadataHeader()
    {
      auto const timestamp = currentTimestamp();
      auto libraryId = generateLibraryId();

      if (!libraryId)
      {
        return std::unexpected{libraryId.error()};
      }

      return MetadataHeader{.magic = kMetadataMagic,
                            .libraryVersion = kLibraryVersion,
                            .flags = 0,
                            .createdTime = timestamp,
                            .libraryId = *libraryId};
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

    Result<MetadataHeader> loadMetadataHeader(lmdb::Database const& database, lmdb::ReadTransaction const& transaction)
    {
      auto const optBytes = database.reader(transaction).get(kMetadataHeaderRecordId);

      if (!optBytes)
      {
        return makeError(Error::Code::NotFound, "Library metadata header was not found");
      }

      if (optBytes->size() != sizeof(MetadataHeader))
      {
        return makeError(
          Error::Code::CorruptData,
          std::format(
            "Invalid library metadata header size {} (expected {})", optBytes->size(), sizeof(MetadataHeader)));
      }

      auto header = MetadataHeader{};
      std::memcpy(&header, optBytes->data(), sizeof(header));
      return header;
    }

    Result<std::uint32_t> decodePersistedId(lmdb::Database::Reader::KeyView const key, std::string_view const database)
    {
      if (key.size() != sizeof(std::uint32_t))
      {
        return makeError(
          Error::Code::CorruptData, std::format("{} contains a {}-byte integer key", database, key.size()));
      }

      std::uint32_t value = 0;
      std::memcpy(&value, key.data(), sizeof(value));
      return value;
    }

    Result<> validateTrackDatabases(lmdb::Database const& hotDatabase,
                                    lmdb::Database const& coldDatabase,
                                    lmdb::ReadTransaction const& transaction,
                                    std::size_t const dictionarySize)
    {
      auto const hotReader = hotDatabase.reader(transaction);
      auto const coldReader = coldDatabase.reader(transaction);
      auto hot = hotReader.begin();
      auto cold = coldReader.begin();
      auto const hotEnd = hotReader.end();
      auto const coldEnd = coldReader.end();

      while (hot != hotEnd && cold != coldEnd)
      {
        auto const& [hotKey, hotPayload] = *hot;
        auto const& [coldKey, coldPayload] = *cold;
        auto hotId = decodePersistedId(hotKey, "Hot Track database");
        auto coldId = decodePersistedId(coldKey, "Cold Track database");

        if (!hotId)
        {
          return std::unexpected{hotId.error()};
        }

        if (!coldId)
        {
          return std::unexpected{coldId.error()};
        }

        if (*hotId == 0 || *coldId == 0 || *hotId != *coldId)
        {
          return makeError(
            Error::Code::CorruptData,
            std::format("Hot and cold Track keys do not form matching nonzero pairs: {} and {}", *hotId, *coldId));
        }

        if (auto validation = validateSerializedTrackReferences(hotPayload, coldPayload, dictionarySize); !validation)
        {
          return makeError(Error::Code::CorruptData,
                           std::format("Track {} failed persisted validation: {}", *hotId, validation.error().message));
        }

        ++hot;
        ++cold;
      }

      if (hot != hotEnd || cold != coldEnd)
      {
        return makeError(Error::Code::CorruptData, "Hot and cold Track databases contain different key sets");
      }

      return {};
    }

    Result<> validateListDatabase(lmdb::Database const& database, lmdb::ReadTransaction const& transaction)
    {
      auto const reader = database.reader(transaction);

      for (auto const& [key, payload] : reader)
      {
        auto id = decodePersistedId(key, "List database");

        if (!id)
        {
          return std::unexpected{id.error()};
        }

        if (*id == 0)
        {
          return makeError(Error::Code::CorruptData, "List database contains the reserved id zero");
        }

        if (!ListView{payload}.isValid())
        {
          return makeError(Error::Code::CorruptData, std::format("List {} record is structurally corrupt", *id));
        }
      }

      return {};
    }

    Result<> validateManifestDatabase(lmdb::Database const& database, lmdb::ReadTransaction const& transaction)
    {
      auto const reader = database.reader(transaction);

      for (auto const& [key, payload] : reader)
      {
        if (auto validation = validateFileManifestEntry(key, payload); !validation)
        {
          return std::unexpected{validation.error()};
        }
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

      auto const persistedHeader = loadMetadataHeader(*metadataDb, *initializationTransaction);

      if (persistedHeader)
      {
        if (auto validation = validateMetadataHeader(*persistedHeader); !validation)
        {
          return std::unexpected{validation.error()};
        }
      }
      else if (persistedHeader.error().code != Error::Code::NotFound)
      {
        return std::unexpected{persistedHeader.error()};
      }
      else if (metadataDb->reader(*initializationTransaction).entryCount() != 0 ||
               tracksHotDb->reader(*initializationTransaction).entryCount() != 0 ||
               tracksColdDb->reader(*initializationTransaction).entryCount() != 0 ||
               listsDb->reader(*initializationTransaction).entryCount() != 0 ||
               resourcesDb->reader(*initializationTransaction).entryCount() != 0 ||
               dictionaryDb->reader(*initializationTransaction).entryCount() != 0 ||
               manifestDb->reader(*initializationTransaction).entryCount() != 0)
      {
        return makeError(Error::Code::CorruptData, "Library data exists without a metadata header");
      }

      auto const dictionarySize = dictionaryDb->reader(*initializationTransaction).entryCount();

      if (auto validation =
            validateTrackDatabases(*tracksHotDb, *tracksColdDb, *initializationTransaction, dictionarySize);
          !validation)
      {
        return std::unexpected{validation.error()};
      }

      if (auto validation = validateListDatabase(*listsDb, *initializationTransaction); !validation)
      {
        return std::unexpected{validation.error()};
      }

      if (auto validation = validateManifestDatabase(*manifestDb, *initializationTransaction); !validation)
      {
        return std::unexpected{validation.error()};
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

      auto implPtr = std::move(*impl);

      auto headerResult = implPtr->metadataStore.load(implPtr->initializationTransaction);

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
        auto header = makeMetadataHeader();

        if (!header)
        {
          return std::unexpected{header.error()};
        }

        if (auto result = implPtr->metadataStore.create(implPtr->initializationTransaction, *header); !result)
        {
          return result;
        }
      }

      if (auto result = implPtr->initializationTransaction.commit(); !result)
      {
        return std::unexpected{result.error()};
      }

      _implPtr = std::move(implPtr);
      return {};
    }
    catch (lmdb::detail::TransactionFailure const& failure)
    {
      // open() is the sole public recoverable constructor and must not leak the
      // lexical transaction-unwind marker to its caller. The local Impl candidate
      // has already been destroyed, aborting its initialization transaction.
      return std::unexpected{failure.error()};
    }
    catch (detail::LibraryException const& failure)
    {
      return std::unexpected{failure.error()};
    }
    catch (std::filesystem::filesystem_error const& ex)
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

    try
    {
      _implPtr->metadataStore.bumpRevision(transaction->native(_implPtr->identity));
    }
    catch (lmdb::detail::TransactionFailure const& failure)
    {
      transaction->abort();
      throwException<Exception>("Failed to initialize write transaction: {}", failure.error().message);
    }
    catch (...)
    {
      transaction->abort();
      throw;
    }

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
