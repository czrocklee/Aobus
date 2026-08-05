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
      auto libraryIdRes = generateLibraryId();

      if (!libraryIdRes)
      {
        return std::unexpected{libraryIdRes.error()};
      }

      return MetadataHeader{.magic = kMetadataMagic,
                            .libraryVersion = kLibraryVersion,
                            .flags = 0,
                            .createdTime = timestamp,
                            .libraryId = *libraryIdRes};
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
        auto hotIdRes = decodePersistedId(hotKey, "Hot Track database");
        auto coldIdRes = decodePersistedId(coldKey, "Cold Track database");

        if (!hotIdRes)
        {
          return std::unexpected{hotIdRes.error()};
        }

        if (!coldIdRes)
        {
          return std::unexpected{coldIdRes.error()};
        }

        if (*hotIdRes == 0 || *coldIdRes == 0 || *hotIdRes != *coldIdRes)
        {
          return makeError(
            Error::Code::CorruptData,
            std::format(
              "Hot and cold Track keys do not form matching nonzero pairs: {} and {}", *hotIdRes, *coldIdRes));
        }

        if (auto validationRes = validateSerializedTrackReferences(hotPayload, coldPayload, dictionarySize);
            !validationRes)
        {
          return makeError(
            Error::Code::CorruptData,
            std::format("Track {} failed persisted validation: {}", *hotIdRes, validationRes.error().message));
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
        auto idRes = decodePersistedId(key, "List database");

        if (!idRes)
        {
          return std::unexpected{idRes.error()};
        }

        if (*idRes == 0)
        {
          return makeError(Error::Code::CorruptData, "List database contains the reserved id zero");
        }

        if (!ListView{payload}.isValid())
        {
          return makeError(Error::Code::CorruptData, std::format("List {} record is structurally corrupt", *idRes));
        }
      }

      return {};
    }

    Result<> validateManifestDatabase(lmdb::Database const& database, lmdb::ReadTransaction const& transaction)
    {
      auto const reader = database.reader(transaction);

      for (auto const& [key, payload] : reader)
      {
        if (auto validationRes = validateFileManifestEntry(key, payload); !validationRes)
        {
          return std::unexpected{validationRes.error()};
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

      auto envRes = lmdb::Environment::open(
        databasePath.string(),
        lmdb::Environment::Options{
          .flags = lmdb::kEnvNoTls, .mode = kLmdbFileMode, .maxDatabases = kLmdbMaxDatabases, .mapSize = mapSize});

      if (!envRes)
      {
        return std::unexpected{envRes.error()};
      }

      auto initializationTransactionRes = lmdb::WriteTransaction::begin(*envRes);

      if (!initializationTransactionRes)
      {
        return std::unexpected{initializationTransactionRes.error()};
      }

      auto metadataDbRes = lmdb::Database::open(*initializationTransactionRes, "meta");
      auto tracksHotDbRes = lmdb::Database::open(*initializationTransactionRes, "tracks_hot");
      auto tracksColdDbRes = lmdb::Database::open(*initializationTransactionRes, "tracks_cold");
      auto listsDbRes = lmdb::Database::open(*initializationTransactionRes, "lists");
      auto resourcesDbRes = lmdb::Database::open(*initializationTransactionRes, "resources");
      auto dictionaryDbRes = lmdb::Database::open(*initializationTransactionRes, "dictionary");
      auto manifestDbRes =
        lmdb::Database::open(*initializationTransactionRes, "file_manifest", lmdb::Database::KeyKind::Blob);

      if (!metadataDbRes)
      {
        return std::unexpected{metadataDbRes.error()};
      }

      if (!tracksHotDbRes)
      {
        return std::unexpected{tracksHotDbRes.error()};
      }

      if (!tracksColdDbRes)
      {
        return std::unexpected{tracksColdDbRes.error()};
      }

      if (!listsDbRes)
      {
        return std::unexpected{listsDbRes.error()};
      }

      if (!resourcesDbRes)
      {
        return std::unexpected{resourcesDbRes.error()};
      }

      if (!dictionaryDbRes)
      {
        return std::unexpected{dictionaryDbRes.error()};
      }

      if (!manifestDbRes)
      {
        return std::unexpected{manifestDbRes.error()};
      }

      auto const persistedHeaderRes = loadMetadataHeader(*metadataDbRes, *initializationTransactionRes);

      if (persistedHeaderRes)
      {
        if (auto validationRes = validateMetadataHeader(*persistedHeaderRes); !validationRes)
        {
          return std::unexpected{validationRes.error()};
        }
      }
      else if (persistedHeaderRes.error().code != Error::Code::NotFound)
      {
        return std::unexpected{persistedHeaderRes.error()};
      }
      else if (metadataDbRes->reader(*initializationTransactionRes).entryCount() != 0 ||
               tracksHotDbRes->reader(*initializationTransactionRes).entryCount() != 0 ||
               tracksColdDbRes->reader(*initializationTransactionRes).entryCount() != 0 ||
               listsDbRes->reader(*initializationTransactionRes).entryCount() != 0 ||
               resourcesDbRes->reader(*initializationTransactionRes).entryCount() != 0 ||
               dictionaryDbRes->reader(*initializationTransactionRes).entryCount() != 0 ||
               manifestDbRes->reader(*initializationTransactionRes).entryCount() != 0)
      {
        return makeError(Error::Code::CorruptData, "Library data exists without a metadata header");
      }

      auto const dictionarySize = dictionaryDbRes->reader(*initializationTransactionRes).entryCount();

      if (auto validationRes =
            validateTrackDatabases(*tracksHotDbRes, *tracksColdDbRes, *initializationTransactionRes, dictionarySize);
          !validationRes)
      {
        return std::unexpected{validationRes.error()};
      }

      if (auto validationRes = validateListDatabase(*listsDbRes, *initializationTransactionRes); !validationRes)
      {
        return std::unexpected{validationRes.error()};
      }

      if (auto validationRes = validateManifestDatabase(*manifestDbRes, *initializationTransactionRes); !validationRes)
      {
        return std::unexpected{validationRes.error()};
      }

      return std::make_unique<Impl>(std::move(musicRoot),
                                    std::move(databasePath),
                                    std::move(*envRes),
                                    std::move(*initializationTransactionRes),
                                    std::move(*metadataDbRes),
                                    std::move(*tracksHotDbRes),
                                    std::move(*tracksColdDbRes),
                                    std::move(*listsDbRes),
                                    std::move(*resourcesDbRes),
                                    std::move(*dictionaryDbRes),
                                    std::move(*manifestDbRes));
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
      auto implRes = Impl::create(std::move(musicRoot), std::move(databasePath), options.mapSize);

      if (!implRes)
      {
        return std::unexpected{implRes.error()};
      }

      auto implPtr = std::move(*implRes);

      auto headerRes = implPtr->metadataStore.load(implPtr->initializationTransaction);

      if (!headerRes && headerRes.error().code != Error::Code::NotFound)
      {
        return std::unexpected{headerRes.error()};
      }

      if (headerRes)
      {
        if (auto result = validateMetadataHeader(*headerRes); !result)
        {
          return std::unexpected{result.error()};
        }
      }
      else
      {
        auto newHeaderRes = makeMetadataHeader();

        if (!newHeaderRes)
        {
          return std::unexpected{newHeaderRes.error()};
        }

        if (auto result = implPtr->metadataStore.create(implPtr->initializationTransaction, *newHeaderRes); !result)
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
    auto transactionRes = lmdb::ReadTransaction::begin(_implPtr->env);

    if (!transactionRes)
    {
      throwException<Exception>("Failed to begin read transaction: {}", transactionRes.error().message);
    }

    return ReadTransaction{std::move(*transactionRes), _implPtr->identity};
  }

  WriteTransaction MusicLibrary::beginWriteTransaction(WriteTransaction::Options options,
                                                       std::shared_ptr<void const> writerSessionAnchorPtr)
  {
    auto transactionRes = WriteTransaction::begin(
      _implPtr->env, _implPtr->dictionary, _implPtr->identity, std::move(options), std::move(writerSessionAnchorPtr));

    if (!transactionRes)
    {
      throwException<Exception>("Failed to begin write transaction: {}", transactionRes.error().message);
    }

    try
    {
      _implPtr->metadataStore.bumpRevision(transactionRes->native(_implPtr->identity));
    }
    catch (lmdb::detail::TransactionFailure const& failure)
    {
      transactionRes->abort();
      throwException<Exception>("Failed to initialize write transaction: {}", failure.error().message);
    }
    catch (...)
    {
      transactionRes->abort();
      throw;
    }

    return std::move(*transactionRes);
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
    auto headerRes = _implPtr->metadataStore.load(transaction);

    if (!headerRes)
    {
      throwException<Exception>("Failed to load library metadata header: {}", headerRes.error().message);
    }

    return *headerRes;
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
