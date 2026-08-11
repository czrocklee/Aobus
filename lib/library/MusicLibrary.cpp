// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/MusicLibrary.h>

#include "FileManifestValidation.h"
#include "LibraryIdentity.h"
#include "ListRecordValidation.h"
#include "MetadataState.h"
#include "MetadataStore.h"
#include "OpenValidationMetrics.h"
#include "TrackRecordValidation.h"
#include "detail/LibraryError.h"
#include "lmdb/detail/TransactionFailure.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

    constexpr auto kCurrentDatabaseNames = std::to_array<std::string_view>(
      {"meta", "tracks_hot", "tracks_cold", "lists", "resources", "dictionary", "file_manifest"});

    struct MetadataPrefix final
    {
      std::uint32_t magic = 0;
      std::uint32_t libraryVersion = 0;
    };

    static_assert(sizeof(MetadataPrefix) == 8);

    struct SchemaDatabases final
    {
      lmdb::Database metadata;
      lmdb::Database tracksHot;
      lmdb::Database tracksCold;
      lmdb::Database lists;
      lmdb::Database resources;
      lmdb::Database dictionary;
      lmdb::Database manifest;
    };

    Result<std::span<std::byte const>> loadMetadataHeaderBytes(lmdb::Database const& database,
                                                               lmdb::ReadTransaction const& transaction)
    {
      auto const optBytes = database.reader(transaction).get(kMetadataHeaderRecordId);

      if (!optBytes)
      {
        return makeError(Error::Code::CorruptData, "Library metadata header was not found");
      }

      return *optBytes;
    }

    Result<MetadataPrefix> validateMetadataPrefix(std::span<std::byte const> const bytes)
    {
      if (bytes.size() < sizeof(MetadataPrefix))
      {
        return makeError(
          Error::Code::CorruptData,
          std::format("Library metadata header is shorter than the stable {}-byte prefix", sizeof(MetadataPrefix)));
      }

      auto prefix = MetadataPrefix{};
      std::memcpy(&prefix, bytes.data(), sizeof(prefix));

      if (prefix.magic != kMetadataMagic)
      {
        return makeError(
          Error::Code::CorruptData,
          std::format("Invalid library metadata magic 0x{:08x} (expected 0x{:08x})", prefix.magic, kMetadataMagic));
      }

      return prefix;
    }

    Result<MetadataHeader> validateCurrentMetadataHeader(std::span<std::byte const> const bytes)
    {
      if (bytes.size() != sizeof(MetadataHeader))
      {
        return makeError(
          Error::Code::CorruptData,
          std::format("Invalid library metadata header size {} (expected {})", bytes.size(), sizeof(MetadataHeader)));
      }

      auto header = MetadataHeader{};
      std::memcpy(&header, bytes.data(), sizeof(header));

      if (header.flags != 0)
      {
        return makeError(Error::Code::CorruptData,
                         std::format("Library metadata header has unsupported flags 0x{:08x}", header.flags));
      }

      return header;
    }

    bool catalogKeyEquals(lmdb::Database::Reader::KeyView const key, std::string_view const expected) noexcept
    {
      return key.size() == expected.size() && std::ranges::equal(key, std::as_bytes(std::span{expected}));
    }

    bool catalogIsEmpty(lmdb::Database const& mainDatabase, lmdb::WriteTransaction const& transaction)
    {
      return mainDatabase.reader(transaction).entryCount() == 0;
    }

    Result<> requireMetadataCatalogEntry(lmdb::Database const& mainDatabase, lmdb::WriteTransaction const& transaction)
    {
      for (auto const& [key, value] : mainDatabase.reader(transaction))
      {
        std::ignore = value;

        if (catalogKeyEquals(key, "meta"))
        {
          return {};
        }
      }

      return makeError(Error::Code::CorruptData, "Nonempty library environment has no meta database");
    }

    Result<> validateCurrentCatalog(lmdb::Database const& mainDatabase, lmdb::WriteTransaction const& transaction)
    {
      auto seen = std::array<bool, kCurrentDatabaseNames.size()>{};
      std::size_t count = 0;

      for (auto const& [key, value] : mainDatabase.reader(transaction))
      {
        std::ignore = value;
        ++count;
        bool matched = false;

        for (std::size_t index = 0; index < kCurrentDatabaseNames.size(); ++index)
        {
          if (catalogKeyEquals(key, kCurrentDatabaseNames[index]))
          {
            seen[index] = true;
            matched = true;
            break;
          }
        }

        if (!matched)
        {
          return makeError(Error::Code::CorruptData, "Library main catalog contains an unknown entry");
        }
      }

      if (count != kCurrentDatabaseNames.size() || !std::ranges::all_of(seen, std::identity{}))
      {
        return makeError(Error::Code::CorruptData, "Library main catalog does not contain the exact current schema");
      }

      return {};
    }

    Result<lmdb::Database> openSchemaDatabase(lmdb::WriteTransaction& transaction,
                                              bool const create,
                                              std::string const& name,
                                              lmdb::Database::KeyKind const kind)
    {
      detail::recordOpenValidationNamedDatabaseOpen();
      return create ? lmdb::Database::open(transaction, name, kind)
                    : lmdb::Database::openExisting(transaction, name, kind);
    }

    Result<SchemaDatabases> openSchemaWithMetadata(lmdb::WriteTransaction& transaction,
                                                   bool const create,
                                                   lmdb::Database metadata)
    {
      if (auto validationRes = metadata.validateExactKeyKind("meta", lmdb::Database::KeyKind::Integer); !validationRes)
      {
        return std::unexpected{validationRes.error()};
      }

      auto tracksHotRes = openSchemaDatabase(transaction, create, "tracks_hot", lmdb::Database::KeyKind::Integer);
      auto tracksColdRes = openSchemaDatabase(transaction, create, "tracks_cold", lmdb::Database::KeyKind::Integer);
      auto listsRes = openSchemaDatabase(transaction, create, "lists", lmdb::Database::KeyKind::Integer);
      auto resourcesRes = openSchemaDatabase(transaction, create, "resources", lmdb::Database::KeyKind::Integer);
      auto dictionaryRes = openSchemaDatabase(transaction, create, "dictionary", lmdb::Database::KeyKind::Integer);
      auto manifestRes = openSchemaDatabase(transaction, create, "file_manifest", lmdb::Database::KeyKind::Blob);

      for (auto const* result : {&tracksHotRes, &tracksColdRes, &listsRes, &resourcesRes, &dictionaryRes, &manifestRes})
      {
        if (!*result)
        {
          return std::unexpected{result->error()};
        }
      }

      return SchemaDatabases{.metadata = std::move(metadata),
                             .tracksHot = std::move(*tracksHotRes),
                             .tracksCold = std::move(*tracksColdRes),
                             .lists = std::move(*listsRes),
                             .resources = std::move(*resourcesRes),
                             .dictionary = std::move(*dictionaryRes),
                             .manifest = std::move(*manifestRes)};
    }

    Result<SchemaDatabases> openSchema(lmdb::WriteTransaction& transaction, bool const create)
    {
      auto metadataRes = openSchemaDatabase(transaction, create, "meta", lmdb::Database::KeyKind::Integer);

      if (!metadataRes)
      {
        return std::unexpected{metadataRes.error()};
      }

      return openSchemaWithMetadata(transaction, create, std::move(*metadataRes));
    }

    struct AdmittedSchema final
    {
      MetadataHeader header;
      std::uint64_t revision = 0;
      SchemaDatabases databases;
    };

    Result<std::uint64_t> validateMetadataDatabase(lmdb::Database const& database,
                                                   lmdb::ReadTransaction const& transaction);

    Result<AdmittedSchema> createFreshSchema(lmdb::WriteTransaction& transaction)
    {
      auto schemaRes = openSchema(transaction, true);

      if (!schemaRes)
      {
        return std::unexpected{schemaRes.error()};
      }

      auto headerRes = makeMetadataHeader();

      if (!headerRes)
      {
        return std::unexpected{headerRes.error()};
      }

      auto createRes =
        schemaRes->metadata.writer(transaction).create(kMetadataHeaderRecordId, utility::bytes::view(*headerRes));

      if (!createRes)
      {
        return std::unexpected{createRes.error()};
      }

      return AdmittedSchema{.header = *headerRes, .revision = 0, .databases = std::move(*schemaRes)};
    }

    Result<AdmittedSchema> admitCurrentSchema(lmdb::Database const& mainDatabase, lmdb::WriteTransaction& transaction)
    {
      if (auto metadataEntryRes = requireMetadataCatalogEntry(mainDatabase, transaction); !metadataEntryRes)
      {
        return std::unexpected{metadataEntryRes.error()};
      }

      detail::recordOpenValidationNamedDatabaseOpen();
      auto rawMetadataRes = lmdb::Database::openExisting(transaction, "meta");

      if (!rawMetadataRes)
      {
        return std::unexpected{rawMetadataRes.error()};
      }

      auto headerBytesRes = loadMetadataHeaderBytes(*rawMetadataRes, transaction);

      if (!headerBytesRes)
      {
        return std::unexpected{headerBytesRes.error()};
      }

      auto prefixRes = validateMetadataPrefix(*headerBytesRes);

      if (!prefixRes)
      {
        return std::unexpected{prefixRes.error()};
      }

      if (prefixRes->libraryVersion != kLibraryVersion)
      {
        return makeError(
          Error::Code::NotSupported,
          std::format("Unsupported library version {} (current {})", prefixRes->libraryVersion, kLibraryVersion));
      }

      if (auto catalogRes = validateCurrentCatalog(mainDatabase, transaction); !catalogRes)
      {
        return std::unexpected{catalogRes.error()};
      }

      auto headerRes = validateCurrentMetadataHeader(*headerBytesRes);

      if (!headerRes)
      {
        return std::unexpected{headerRes.error()};
      }

      auto schemaRes = openSchemaWithMetadata(transaction, false, std::move(*rawMetadataRes));

      if (!schemaRes)
      {
        return std::unexpected{schemaRes.error()};
      }

      auto revisionRes = validateMetadataDatabase(schemaRes->metadata, transaction);

      if (!revisionRes)
      {
        return std::unexpected{revisionRes.error()};
      }

      return AdmittedSchema{.header = *headerRes, .revision = *revisionRes, .databases = std::move(*schemaRes)};
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

    Result<std::uint64_t> validateMetadataDatabase(lmdb::Database const& database,
                                                   lmdb::ReadTransaction const& transaction)
    {
      auto const reader = database.reader(transaction);
      bool foundHeader = false;
      std::uint64_t revision = 0;

      for (auto const& [key, payload] : reader)
      {
        auto idRes = decodePersistedId(key, "Metadata database");

        if (!idRes)
        {
          return std::unexpected{idRes.error()};
        }

        if (*idRes == kMetadataHeaderRecordId)
        {
          if (auto headerRes = validateCurrentMetadataHeader(payload); !headerRes)
          {
            return std::unexpected{headerRes.error()};
          }

          foundHeader = true;
          continue;
        }

        if (*idRes != kLibraryRevisionRecordId)
        {
          return makeError(
            Error::Code::CorruptData, std::format("Metadata database contains unknown record {}", *idRes));
        }

        if (payload.size() != sizeof(revision))
        {
          return makeError(
            Error::Code::CorruptData,
            std::format("Library revision record has size {} (expected {})", payload.size(), sizeof(revision)));
        }

        std::memcpy(&revision, payload.data(), sizeof(revision));

        if (revision == 0 || revision == std::numeric_limits<std::uint64_t>::max())
        {
          return makeError(
            Error::Code::CorruptData, std::format("Library revision record contains reserved value {}", revision));
        }
      }

      if (!foundHeader)
      {
        return makeError(Error::Code::CorruptData, "Metadata database has no header record");
      }

      return revision;
    }

    Result<> validateResourceDatabase(lmdb::Database const& database, lmdb::ReadTransaction const& transaction)
    {
      for (auto const& [key, payload] : database.reader(transaction))
      {
        auto idRes = decodePersistedId(key, "Resource database");

        if (!idRes)
        {
          return std::unexpected{idRes.error()};
        }

        if (*idRes == 0)
        {
          return makeError(Error::Code::CorruptData, "Resource database contains the reserved id zero");
        }

        if (payload.empty())
        {
          return makeError(Error::Code::CorruptData, std::format("Resource {} contains an empty value", *idRes));
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

    Result<> validateTrackDatabases(lmdb::Database const& hotDatabase,
                                    lmdb::Database const& coldDatabase,
                                    lmdb::Database const& resourceDatabase,
                                    lmdb::Database const& manifestDatabase,
                                    lmdb::ReadTransaction const& transaction,
                                    std::size_t const dictionarySize)
    {
      auto const hotReader = hotDatabase.reader(transaction);
      auto const coldReader = coldDatabase.reader(transaction);
      auto const resourceReader = resourceDatabase.reader(transaction);
      auto const manifestReader = manifestDatabase.reader(transaction);

      if (hotReader.entryCount() != manifestReader.entryCount())
      {
        return makeError(Error::Code::CorruptData, "Track and file manifest databases contain different row counts");
      }

      auto hot = hotReader.begin();
      auto cold = coldReader.begin();
      auto const hotEnd = hotReader.end();
      auto const coldEnd = coldReader.end();

      while (hot != hotEnd && cold != coldEnd)
      {
        detail::recordOpenValidationTrackRow();
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

        auto const view = TrackView{hotPayload, coldPayload};

        for (auto const cover : view.coverArt())
        {
          if (!resourceReader.get(cover.resourceId.raw()))
          {
            return makeError(Error::Code::CorruptData,
                             std::format("Track {} references missing Resource {}", *hotIdRes, cover.resourceId.raw()));
          }
        }

        auto const uri = view.property().uri();
        auto const manifestKey = detail::PaddedFileManifestKey{uri};
        detail::recordOpenValidationManifestPointRead();
        auto const optManifestPayload = manifestReader.get(manifestKey.bytes());

        if (!optManifestPayload)
        {
          return makeError(
            Error::Code::CorruptData, std::format("Track {} has no file manifest row for '{}'", *hotIdRes, uri));
        }

        auto const manifestView = FileManifestView{*optManifestPayload};

        if (!manifestView.isValid() || manifestView.trackId().raw() != *hotIdRes)
        {
          return makeError(Error::Code::CorruptData,
                           std::format("Track {} has a mismatched file manifest binding for '{}'", *hotIdRes, uri));
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
      enum class Color : std::uint8_t
      {
        White,
        Gray,
        Black
      };
      struct Node final
      {
        std::uint32_t parentId = 0;
        Color color = Color::White;
      };
      auto nodes = std::unordered_map<std::uint32_t, Node>{};
      nodes.reserve(reader.entryCount());

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

        if (auto validationRes = validateSerializedList(payload); !validationRes)
        {
          return makeError(
            Error::Code::CorruptData,
            std::format("List {} failed persisted validation: {}", *idRes, validationRes.error().message));
        }

        auto const [it, inserted] = nodes.emplace(*idRes, Node{.parentId = ListView{payload}.parentId().raw()});
        std::ignore = it;
        AO_INVARIANT(inserted, "LMDB yielded a duplicate List key");
      }

      for (auto const& [id, node] : nodes)
      {
        if (node.parentId != 0 && !nodes.contains(node.parentId))
        {
          return makeError(
            Error::Code::CorruptData, std::format("List {} references missing parent {}", id, node.parentId));
        }
      }

      auto path = std::vector<std::uint32_t>{};
      path.reserve(nodes.size());

      for (auto const& [startId, startNode] : nodes)
      {
        if (startNode.color != Color::White)
        {
          continue;
        }

        path.clear();
        auto currentId = startId;

        while (currentId != 0)
        {
          auto& current = nodes.at(currentId);

          if (current.color == Color::Black)
          {
            break;
          }

          if (current.color == Color::Gray)
          {
            return makeError(
              Error::Code::CorruptData, std::format("List parent graph contains a cycle through {}", currentId));
          }

          current.color = Color::Gray;
          path.push_back(currentId);
          currentId = current.parentId;
        }

        for (auto const id : path)
        {
          nodes.at(id).color = Color::Black;
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
    detail::MetadataState metadataState;
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
         MetadataHeader metadataHeader,
         std::uint64_t committedRevision,
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
      , metadataState{metadataHeader, committedRevision}
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
      detail::resetOpenValidationMetrics();

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

      auto const mainDatabase = lmdb::Database::main(*initializationTransactionRes);
      auto const catalogEmpty = catalogIsEmpty(mainDatabase, *initializationTransactionRes);
      auto admittedSchemaRes = catalogEmpty ? createFreshSchema(*initializationTransactionRes)
                                            : admitCurrentSchema(mainDatabase, *initializationTransactionRes);

      if (!admittedSchemaRes)
      {
        return std::unexpected{admittedSchemaRes.error()};
      }

      auto admittedSchema = std::move(*admittedSchemaRes);
      auto& schema = admittedSchema.databases;

      auto implPtr = std::make_unique<Impl>(std::move(musicRoot),
                                            std::move(databasePath),
                                            std::move(*envRes),
                                            std::move(*initializationTransactionRes),
                                            admittedSchema.header,
                                            admittedSchema.revision,
                                            schema.metadata,
                                            schema.tracksHot,
                                            schema.tracksCold,
                                            schema.lists,
                                            schema.resources,
                                            schema.dictionary,
                                            schema.manifest);

      if (auto validationRes = validateResourceDatabase(schema.resources, implPtr->initializationTransaction);
          !validationRes)
      {
        return std::unexpected{validationRes.error()};
      }

      if (auto validationRes = validateManifestDatabase(schema.manifest, implPtr->initializationTransaction);
          !validationRes)
      {
        return std::unexpected{validationRes.error()};
      }

      if (auto validationRes = validateTrackDatabases(schema.tracksHot,
                                                      schema.tracksCold,
                                                      schema.resources,
                                                      schema.manifest,
                                                      implPtr->initializationTransaction,
                                                      implPtr->dictionary.size());
          !validationRes)
      {
        return std::unexpected{validationRes.error()};
      }

      if (auto validationRes = validateListDatabase(schema.lists, implPtr->initializationTransaction); !validationRes)
      {
        return std::unexpected{validationRes.error()};
      }

      return implPtr;
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
      AO_FATAL("Failed to begin library read transaction: {}", transactionRes.error().message);
    }

    auto headerRes = _implPtr->metadataStore.load(*transactionRes);
    AO_INVARIANT(headerRes, "Library metadata header failed after open validation: {}", headerRes.error().message);
    auto const revision = _implPtr->metadataStore.revision(*transactionRes);
    return ReadTransaction{std::move(*transactionRes), _implPtr->identity, *headerRes, revision};
  }

  WriteTransaction MusicLibrary::beginWriteTransaction(WriteTransaction::Options options,
                                                       std::shared_ptr<void const> writerSessionAnchorPtr)
  {
    auto transactionRes = WriteTransaction::begin(_implPtr->env,
                                                  _implPtr->tracks,
                                                  _implPtr->lists,
                                                  _implPtr->resources,
                                                  _implPtr->dictionary,
                                                  _implPtr->manifest,
                                                  _implPtr->metadataStore,
                                                  _implPtr->metadataState,
                                                  _implPtr->identity,
                                                  std::move(options),
                                                  std::move(writerSessionAnchorPtr));

    if (!transactionRes)
    {
      AO_FATAL("Failed to begin library write transaction: {}", transactionRes.error().message);
    }

    return std::move(*transactionRes);
  }

  std::uint64_t MusicLibrary::libraryRevision(ReadTransaction const& transaction) const
  {
    AO_EXPECTS(transaction._identity == &_implPtr->identity, "Read transaction belongs to a different MusicLibrary");
    return transaction._libraryRevision;
  }

  std::uint64_t MusicLibrary::libraryRevision(WriteTransaction const& transaction) const
  {
    return transaction.libraryRevision();
  }

  std::uint64_t MusicLibrary::libraryRevision(LibraryWrite const& write) const
  {
    write._transaction->requireOperationActive();
    return write._transaction->libraryRevision();
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

  MetadataHeader MusicLibrary::metadataHeader() const
  {
    return _implPtr->metadataState.snapshot().header;
  }

  MetadataHeader MusicLibrary::metadataHeader(ReadTransaction const& transaction) const
  {
    AO_EXPECTS(transaction._identity == &_implPtr->identity, "Read transaction belongs to a different MusicLibrary");
    return transaction._metadataHeader;
  }

  MetadataHeader MusicLibrary::metadataHeader(WriteTransaction const& transaction) const
  {
    return transaction.metadataHeader();
  }

  MetadataHeader MusicLibrary::metadataHeader(LibraryWrite const& write) const
  {
    write._transaction->requireOperationActive();
    return write._transaction->metadataHeader();
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
