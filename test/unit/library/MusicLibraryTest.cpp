// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/MusicLibrary.h>

#include "lib/library/FileManifestValidation.h"
#include "lib/library/OpenValidationMetrics.h"
#include "lib/lmdb/detail/ReadFaultInjection.h"
#include "test/fatal/ProbeProcess.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  namespace
  {
    enum class RawKeyKind : std::uint8_t
    {
      Integer,
      Byte
    };

    void createLibraryMetadataHeader(std::filesystem::path const& path, std::uint32_t libraryVersion)
    {
      auto env = lmdb::test::openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = lmdb::test::beginWriteTransaction(env);
      auto metadataDatabase = lmdb::test::openIntegerKeyDatabase(transaction, "meta");
      auto header = MetadataHeader{.magic = kMetadataMagic,
                                   .libraryVersion = libraryVersion,
                                   .flags = 0,
                                   .createdTime = std::chrono::sys_time{std::chrono::milliseconds{1}}};
      REQUIRE(metadataDatabase.writer(transaction).create(kMetadataHeaderRecordId, utility::bytes::view(header)));
      REQUIRE(transaction.commit());
    }

    void initializeLibrary(std::filesystem::path const& path)
    {
      auto const libraryRes = openTestMusicLibrary(path, path);
      REQUIRE(libraryRes);
    }

    void createRawIntegerRow(std::filesystem::path const& path,
                             std::string const& databaseName,
                             std::uint32_t const id,
                             std::span<std::byte const> const payload)
    {
      auto environment = openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = beginWriteTransaction(environment);
      auto database = openIntegerKeyDatabase(transaction, databaseName);
      REQUIRE(database.writer(transaction).create(id, payload));
      REQUIRE(transaction.commit());
    }

    void createRawIntegerKeyRow(std::filesystem::path const& path,
                                std::string const& databaseName,
                                std::span<std::byte const> const key,
                                std::span<std::byte const> const payload)
    {
      auto* rawEnvironment = static_cast<MDB_env*>(nullptr);
      REQUIRE(::mdb_env_create(&rawEnvironment) == MDB_SUCCESS);
      auto environmentPtr = std::unique_ptr<MDB_env, decltype(&::mdb_env_close)>{rawEnvironment, &::mdb_env_close};
      REQUIRE(::mdb_env_set_maxdbs(environmentPtr.get(), 8) == MDB_SUCCESS);
      REQUIRE(::mdb_env_open(environmentPtr.get(), path.string().c_str(), MDB_NOTLS, 0644) == MDB_SUCCESS);
      auto* rawTransaction = static_cast<MDB_txn*>(nullptr);
      REQUIRE(::mdb_txn_begin(environmentPtr.get(), nullptr, 0, &rawTransaction) == MDB_SUCCESS);
      auto transactionPtr = std::unique_ptr<MDB_txn, decltype(&::mdb_txn_abort)>{rawTransaction, &::mdb_txn_abort};
      MDB_dbi dbi = 0;
      REQUIRE(::mdb_dbi_open(transactionPtr.get(), databaseName.c_str(), MDB_INTEGERKEY, &dbi) == MDB_SUCCESS);
      auto mutableKey = std::vector<std::byte>{key.begin(), key.end()};
      auto mutablePayload = std::vector<std::byte>{payload.begin(), payload.end()};
      auto nativeKey = MDB_val{.mv_size = mutableKey.size(), .mv_data = mutableKey.data()};
      auto nativePayload = MDB_val{.mv_size = mutablePayload.size(), .mv_data = mutablePayload.data()};
      REQUIRE(::mdb_put(transactionPtr.get(), dbi, &nativeKey, &nativePayload, MDB_NOOVERWRITE) == MDB_SUCCESS);
      auto* commitTransaction = transactionPtr.release();
      REQUIRE(::mdb_txn_commit(commitTransaction) == MDB_SUCCESS);
    }

    void createRawBlobRow(std::filesystem::path const& path,
                          std::string const& databaseName,
                          std::span<std::byte const> const key,
                          std::span<std::byte const> const payload)
    {
      auto environment = openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = beginWriteTransaction(environment);
      auto database = openByteKeyDatabase(transaction, databaseName);
      REQUIRE(database.writer(transaction).create(key, payload));
      REQUIRE(transaction.commit());
    }

    void updateRawIntegerRow(std::filesystem::path const& path,
                             std::string const& databaseName,
                             std::uint32_t const id,
                             std::span<std::byte const> const payload)
    {
      auto environment = openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = beginWriteTransaction(environment);
      auto database = openIntegerKeyDatabase(transaction, databaseName);
      REQUIRE(database.writer(transaction).update(id, payload));
      REQUIRE(transaction.commit());
    }

    void createRawManifestRow(std::filesystem::path const& path, std::string_view const uri, TrackId const trackId)
    {
      auto const key = detail::PaddedFileManifestKey{uri};
      auto const payload = FileManifestBuilder::makeEmpty().trackId(trackId).serialize();
      createRawBlobRow(path, "file_manifest", key.bytes(), payload);
    }

    void dropNamedDatabase(std::filesystem::path const& path,
                           std::string const& databaseName,
                           std::optional<RawKeyKind> const optReplacementKind = std::nullopt)
    {
      auto* rawEnvironment = static_cast<MDB_env*>(nullptr);
      REQUIRE(::mdb_env_create(&rawEnvironment) == MDB_SUCCESS);
      auto environmentPtr = std::unique_ptr<MDB_env, decltype(&::mdb_env_close)>{rawEnvironment, &::mdb_env_close};
      REQUIRE(::mdb_env_set_maxdbs(environmentPtr.get(), 8) == MDB_SUCCESS);
      REQUIRE(::mdb_env_open(environmentPtr.get(), path.string().c_str(), MDB_NOTLS, 0644) == MDB_SUCCESS);
      auto* rawTransaction = static_cast<MDB_txn*>(nullptr);
      REQUIRE(::mdb_txn_begin(environmentPtr.get(), nullptr, 0, &rawTransaction) == MDB_SUCCESS);
      auto transactionPtr = std::unique_ptr<MDB_txn, decltype(&::mdb_txn_abort)>{rawTransaction, &::mdb_txn_abort};
      MDB_dbi dbi = 0;
      REQUIRE(::mdb_dbi_open(transactionPtr.get(), databaseName.c_str(), 0, &dbi) == MDB_SUCCESS);
      REQUIRE(::mdb_drop(transactionPtr.get(), dbi, 1) == MDB_SUCCESS);

      if (optReplacementKind)
      {
        std::uint32_t flags = MDB_CREATE;

        if (*optReplacementKind == RawKeyKind::Integer)
        {
          flags |= MDB_INTEGERKEY;
        }

        REQUIRE(::mdb_dbi_open(transactionPtr.get(), databaseName.c_str(), flags, &dbi) == MDB_SUCCESS);
      }

      auto* commitTransaction = transactionPtr.release();
      REQUIRE(::mdb_txn_commit(commitTransaction) == MDB_SUCCESS);
    }

    void addOrdinaryMainCatalogRow(std::filesystem::path const& path, std::string_view const key)
    {
      auto environment = openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = beginWriteTransaction(environment);
      auto mainDatabaseRes = ByteKeyDatabase::main(transaction);
      REQUIRE(mainDatabaseRes);
      auto mainDatabase = std::move(*mainDatabaseRes);
      REQUIRE(mainDatabase.writer(transaction).create(utility::bytes::view(key), createStringData("ordinary")));
      REQUIRE(transaction.commit());
    }

    std::vector<std::byte> makeColdDataWithCover(std::string_view const uri, ResourceId const resourceId)
    {
      auto header = TrackColdHeader{};
      header.blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::CoverArt)] = sizeof(TrackColdHeader);
      header.uriOffset = sizeof(TrackColdHeader) + sizeof(CoverArtEntry);
      header.uriLength = static_cast<std::uint16_t>(uri.size());
      auto const logicalSize = static_cast<std::size_t>(header.uriOffset) + uri.size();
      auto payload = std::vector<std::byte>(alignToWord(logicalSize), std::byte{0});
      auto const cover = CoverArtEntry{.id = resourceId};
      std::memcpy(payload.data(), &header, sizeof(header));
      std::memcpy(payload.data() + sizeof(header), &cover, sizeof(cover));
      std::memcpy(payload.data() + header.uriOffset, uri.data(), uri.size());
      return payload;
    }

    void createRawTrackPair(std::filesystem::path const& path,
                            TrackId const trackId,
                            std::string_view const uri,
                            std::optional<ResourceId> const optCover = std::nullopt)
    {
      createRawIntegerRow(path, "tracks_hot", trackId.raw(), makeHotData());
      auto cold = optCover ? makeColdDataWithCover(uri, *optCover) : makeColdData({}, uri);
      createRawIntegerRow(path, "tracks_cold", trackId.raw(), cold);
    }

    /**
     * @brief A persisted descriptor row whose digest derives @p homeKey.
     *
     * The gate checks placement and uniqueness, never that a digest hashes any
     * real content, so a crafted digest is exactly what these sections need.
     * @p discriminator keeps two rows in one cluster distinct.
     */
    std::array<std::byte, kResourceDescriptorSize> resourceDescriptorRow(std::uint32_t const homeKey,
                                                                         std::uint8_t const discriminator)
    {
      auto digest = utility::Sha256Digest{};
      digest.bytes[0] = static_cast<std::byte>((homeKey >> 24U) & 0xFFU);
      digest.bytes[1] = static_cast<std::byte>((homeKey >> 16U) & 0xFFU);
      digest.bytes[2] = static_cast<std::byte>((homeKey >> 8U) & 0xFFU);
      digest.bytes[3] = static_cast<std::byte>(homeKey & 0xFFU);
      digest.bytes[utility::Sha256Digest::kByteCount - 1] = std::byte{discriminator};
      return std::bit_cast<std::array<std::byte, kResourceDescriptorSize>>(
        ResourceDescriptor{.digest = digest, .byteLength = 1024});
    }

    void requireCorruptLibrary(std::filesystem::path const& path)
    {
      auto const result = openTestMusicLibrary(path, path);
      auto const message = result ? std::string{} : result.error().message;
      INFO(message);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  } // namespace

  TEST_CASE("MusicLibrary - initializes metadata header", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};

    auto const firstHeader = [&]
    {
      auto firstRes = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE(firstRes);
      auto const header = MetadataHeader{firstRes->metadataHeader()};
      CHECK(header.magic == kMetadataMagic);
      CHECK(header.libraryVersion == kLibraryVersion);
      return header;
    }();

    auto reopenedRes = openTestMusicLibrary(temp.path(), temp.path());
    auto const reopenMessage = reopenedRes ? std::string{} : reopenedRes.error().message;
    INFO(reopenMessage);
    REQUIRE(reopenedRes);
    auto const& reopened = *reopenedRes;
    CHECK(reopened.metadataHeader().libraryId == firstHeader.libraryId);
    CHECK(reopened.metadataHeader().createdTime == firstHeader.createdTime);
  }

  TEST_CASE("MusicLibrary - storageCapacity reports the map it was opened with",
            "[library][unit][music-library][capacity]")
  {
    auto const temp = ao::test::TempDir{};
    auto libraryRes = openTestMusicLibrary(temp.path(), temp.path());
    REQUIRE(libraryRes);

    auto const capacity = libraryRes->storageCapacity();
    CHECK(capacity.mapBytes == kTestMusicLibraryMapBytes);
    // A freshly admitted library holds its metadata, so it is past zero pages and
    // nowhere near the map it may grow into.
    CHECK(capacity.highWaterBytes > 0);
    CHECK(capacity.highWaterBytes < capacity.mapBytes);
  }

  TEST_CASE("MusicLibrary - managed capacity opens a fresh library well past the LMDB default",
            "[library][unit][music-library][capacity]")
  {
    constexpr auto kLmdbDefaultMapBytes = std::uint64_t{1} * 1024 * 1024;
    auto const temp = ao::test::TempDir{};

    // No pinned map size, so the library's own floor decides. LMDB would
    // otherwise hand a new database 1 MiB, which one scan passes.
    auto libraryRes = MusicLibrary::open(temp.path(), temp.path() / "managed-db");
    REQUIRE(libraryRes);
    CHECK(libraryRes->storageCapacity().mapBytes > kLmdbDefaultMapBytes * 512);
  }

  TEST_CASE("MusicLibrary - a pinned map size stays pinned across reopening",
            "[library][unit][music-library][capacity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "pinned-db";

    {
      auto libraryRes = openTestMusicLibrary(temp.path(), databasePath);
      REQUIRE(libraryRes);
      REQUIRE(libraryRes->storageCapacity().mapBytes == kTestMusicLibraryMapBytes);
    }

    // Reopening pinned must not inherit-and-grow, or a test could never hold a
    // library at a capacity it means to reach.
    auto libraryRes = openTestMusicLibrary(temp.path(), databasePath);
    REQUIRE(libraryRes);
    CHECK(libraryRes->storageCapacity().mapBytes == kTestMusicLibraryMapBytes);
  }

  TEST_CASE("MusicLibrary - opens each named database once per library admission", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};

    {
      auto library = makeTestMusicLibrary(temp.path(), temp.path());
      CHECK(detail::openValidationMetrics().namedDatabaseOpens == 7);
    }

    auto reopened = makeTestMusicLibrary(temp.path(), temp.path());
    CHECK(detail::openValidationMetrics().namedDatabaseOpens == 7);
  }

  TEST_CASE("MusicLibrary - admits only the exact current main catalog", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("missing named database")
    {
      dropNamedDatabase(temp.path(), "resources");
      requireCorruptLibrary(temp.path());
    }

    SECTION("unknown named database")
    {
      {
        auto environment = openEnvironment(temp.path(), {.flags = MDB_NOTLS, .maxDatabases = 8});
        auto transaction = beginWriteTransaction(environment);
        std::ignore = openIntegerKeyDatabase(transaction, "future_extension");
        REQUIRE(transaction.commit());
      }

      requireCorruptLibrary(temp.path());
    }

    SECTION("ordinary main-database row")
    {
      addOrdinaryMainCatalogRow(temp.path(), "ordinary");
      requireCorruptLibrary(temp.path());
    }

    SECTION("wrong named-database key flags")
    {
      dropNamedDatabase(temp.path(), "resources", RawKeyKind::Byte);
      requireCorruptLibrary(temp.path());
    }

    SECTION("wrong metadata key flags retain exact diagnostics without reopening")
    {
      auto const header = [&]
      {
        auto library = makeTestMusicLibrary(temp.path(), temp.path());
        return library.metadataHeader();
      }();
      dropNamedDatabase(temp.path(), "meta", RawKeyKind::Byte);
      createRawBlobRow(
        temp.path(), "meta", utility::bytes::view(kMetadataHeaderRecordId), utility::bytes::view(header));

      auto const result = openTestMusicLibrary(temp.path(), temp.path());

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
      CHECK(result.error().message == "Named database 'meta' has flags 0x0 (expected 0x8)");
      CHECK(detail::openValidationMetrics().namedDatabaseOpens == 1);
    }
  }

  TEST_CASE("MusicLibrary - does not mistake an ordinary meta row for the metadata database",
            "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    addOrdinaryMainCatalogRow(temp.path(), "meta");
    requireCorruptLibrary(temp.path());
  }

  TEST_CASE("MusicLibrary - rejects persisted data without metadata", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    createRawIntegerRow(temp.path(), "dictionary", 1, createStringData("orphaned"));

    requireCorruptLibrary(temp.path());
  }

  TEST_CASE("MusicLibrary - rejects non-NFC dictionary rows", "[library][unit][music-library][integrity][unicode]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());
    createRawIntegerRow(temp.path(), "dictionary", 1, createStringData("Dvor\u030Ca\u0301k"));

    requireCorruptLibrary(temp.path());
  }

  TEST_CASE("MusicLibrary - reports unsupported library versions as NotSupported", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    constexpr std::uint32_t kLegacyV1LibraryVersion = 1;
    constexpr std::uint32_t kPreviousColdLayoutLibraryVersion = 2;
    constexpr std::uint32_t kPreUnifiedListOrderingLibraryVersion = 4;
    constexpr std::uint32_t kPreNfcTextLibraryVersion = 6;

    SECTION("future version")
    {
      createLibraryMetadataHeader(temp.path(), kLibraryVersion + 1);

      auto const result = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("future version takes precedence over current metadata key flags")
    {
      auto const header = MetadataHeader{.magic = kMetadataMagic, .libraryVersion = kLibraryVersion + 1};
      createRawBlobRow(
        temp.path(), "meta", utility::bytes::view(kMetadataHeaderRecordId), utility::bytes::view(header));

      auto const result = openTestMusicLibrary(temp.path(), temp.path());

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
      CHECK(detail::openValidationMetrics().namedDatabaseOpens == 1);
    }

    SECTION("old version")
    {
      static_assert(kLegacyV1LibraryVersion != kLibraryVersion);
      createLibraryMetadataHeader(temp.path(), kLegacyV1LibraryVersion);

      auto const result = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("previous cold layout version")
    {
      static_assert(kPreviousColdLayoutLibraryVersion != kLibraryVersion);
      createLibraryMetadataHeader(temp.path(), kPreviousColdLayoutLibraryVersion);

      auto const result = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("version 4 before unified List ordering")
    {
      static_assert(kPreUnifiedListOrderingLibraryVersion != kLibraryVersion);
      createLibraryMetadataHeader(temp.path(), kPreUnifiedListOrderingLibraryVersion);

      auto const result = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("version 6 before NFC text admission")
    {
      static_assert(kPreNfcTextLibraryVersion != kLibraryVersion);
      createLibraryMetadataHeader(temp.path(), kPreNfcTextLibraryVersion);

      auto const result = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }
  }

  TEST_CASE("MusicLibrary - future schema is rejected before current exact-shape checks",
            "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    auto header = [&]
    {
      auto library = makeTestMusicLibrary(temp.path(), temp.path());
      return library.metadataHeader();
    }();
    header.libraryVersion = kLibraryVersion + 1U;
    auto enlargedHeader = std::vector<std::byte>(sizeof(header) + 24U, std::byte{0x5a});
    std::memcpy(enlargedHeader.data(), &header, sizeof(header));
    updateRawIntegerRow(temp.path(), "meta", kMetadataHeaderRecordId, enlargedHeader);

    {
      auto environment = openEnvironment(temp.path(), {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = beginWriteTransaction(environment);
      std::ignore = openByteKeyDatabase(transaction, "future_extension");
      REQUIRE(transaction.commit());
    }

    auto const result = openTestMusicLibrary(temp.path(), temp.path());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("MusicLibrary - validates the staged metadata header admission rules",
            "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    auto header = [&]
    {
      auto library = makeTestMusicLibrary(temp.path(), temp.path());
      return library.metadataHeader();
    }();

    SECTION("shorter than stable prefix")
    {
      auto const bytes = std::array<std::byte, 7>{};
      updateRawIntegerRow(temp.path(), "meta", kMetadataHeaderRecordId, bytes);
      requireCorruptLibrary(temp.path());
    }

    SECTION("wrong magic")
    {
      header.magic ^= 1U;
      updateRawIntegerRow(temp.path(), "meta", kMetadataHeaderRecordId, utility::bytes::view(header));
      requireCorruptLibrary(temp.path());
    }

    SECTION("wrong current-version exact size")
    {
      auto bytes = std::vector<std::byte>(sizeof(header) + 1U, std::byte{0});
      std::memcpy(bytes.data(), &header, sizeof(header));
      updateRawIntegerRow(temp.path(), "meta", kMetadataHeaderRecordId, bytes);
      requireCorruptLibrary(temp.path());
    }

    SECTION("nonzero current-version flags")
    {
      header.flags = 1;
      updateRawIntegerRow(temp.path(), "meta", kMetadataHeaderRecordId, utility::bytes::view(header));
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - admits only the current metadata record set and revision range",
            "[library][unit][music-library][revision]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("unknown metadata record")
    {
      createRawIntegerRow(temp.path(), "meta", 99, createStringData("unknown"));
      requireCorruptLibrary(temp.path());
    }

    SECTION("revision with wrong size")
    {
      auto const bytes = std::array<std::byte, sizeof(std::uint64_t) - 1U>{};
      createRawIntegerRow(temp.path(), "meta", kLibraryRevisionRecordId, bytes);
      requireCorruptLibrary(temp.path());
    }

    SECTION("persisted revision zero")
    {
      constexpr std::uint64_t kZeroRevision = 0;
      createRawIntegerRow(temp.path(), "meta", kLibraryRevisionRecordId, utility::bytes::view(kZeroRevision));
      requireCorruptLibrary(temp.path());
    }

    SECTION("reserved maximum revision")
    {
      constexpr auto kReservedRevision = std::numeric_limits<std::uint64_t>::max();
      createRawIntegerRow(temp.path(), "meta", kLibraryRevisionRecordId, utility::bytes::view(kReservedRevision));
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - maximum valid committed revision opens", "[library][unit][music-library][revision]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());
    constexpr auto kMaximumValidRevision = std::numeric_limits<std::uint64_t>::max() - 1U;
    createRawIntegerRow(temp.path(), "meta", kLibraryRevisionRecordId, utility::bytes::view(kMaximumValidRevision));

    auto library = makeTestMusicLibrary(temp.path(), temp.path());
    auto read = library.readTransaction();
    CHECK(library.libraryRevision(read) == kMaximumValidRevision);
  }

  TEST_CASE("MusicLibrary - Track and manifest admission work grows linearly", "[library][unit][music-library][cost]")
  {
    auto const measure = [](std::size_t const trackCount)
    {
      auto const temp = ao::test::TempDir{};

      {
        auto library = makeTestMusicLibrary(temp.path(), temp.path());
        auto transaction = writeTransaction(library);
        auto createRes = transaction.apply(
          [trackCount](LibraryWrite& write) -> Result<>
          {
            auto writer = write.tracks();

            for (std::size_t index = 0; index < trackCount; ++index)
            {
              auto const uri = std::format("linear-{}.flac", index);
              auto track = TrackBuilder::makeEmpty();
              track.property().uri(uri);

              if (auto result = writer.create(track, FileManifestBuilder::makeEmpty()); !result)
              {
                return std::unexpected{result.error()};
              }
            }

            return {};
          });
        REQUIRE(createRes);
        REQUIRE(transaction.commit());
      }

      [[maybe_unused]] auto reopened = makeTestMusicLibrary(temp.path(), temp.path());
      return detail::openValidationMetrics();
    };

    constexpr std::size_t kN = 24;
    auto const atN = measure(kN);
    auto const atTwoN = measure(kN * 2U);

    CHECK(atN.trackCursorRows == kN);
    CHECK(atN.manifestPointReads == kN);
    CHECK(atTwoN.trackCursorRows == 2U * atN.trackCursorRows);
    CHECK(atTwoN.manifestPointReads == 2U * atN.manifestPointReads);
  }

  TEST_CASE("MusicLibrary - open reports a storage fault as a Result", "[library][regression][music-library]")
  {
    // open() is the sole public recoverable constructor. Storage mutations
    // inside it raise TransactionFailure to leave the initialization
    // transaction; that lexical unwind marker must never reach the caller.
    auto const temp = ao::test::TempDir{};
    constexpr std::size_t kUnusableMapSize = std::size_t{8} * 1024;

    auto const result = MusicLibrary::open(
      temp.path(), temp.path() / "tiny-db", MusicLibrary::Options{.pinnedMapBytes = kUnusableMapSize});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::StorageFull);
  }

  TEST_CASE("MusicLibrary - injected validation read fault remains a recoverable open result",
            "[library][regression][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    auto injection = lmdb::detail::ReadFaultInjection{MDB_PANIC};

    auto const result = openTestMusicLibrary(temp.path(), temp.path());

    REQUIRE_FALSE(result);
    CHECK(injection.wasConsumed());
    CHECK(result.error().code == Error::Code::IoError);
    CHECK(result.error().message.contains("mdb_stat"));
  }

  TEST_CASE("MusicLibrary - rejects invalid persisted dictionary state", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("dictionary ids must be dense from one")
    {
      createRawIntegerRow(temp.path(), "dictionary", 2, createStringData("gap"));
      requireCorruptLibrary(temp.path());
    }

    SECTION("dictionary keys must have the native 32-bit width")
    {
      auto const shortKey = std::array{std::byte{1}, std::byte{0}};
      createRawIntegerKeyRow(temp.path(), "dictionary", shortKey, createStringData("short"));
      requireCorruptLibrary(temp.path());
    }

    SECTION("dictionary text must be unique")
    {
      createRawIntegerRow(temp.path(), "dictionary", 1, createStringData("duplicate"));
      createRawIntegerRow(temp.path(), "dictionary", 2, createStringData("duplicate"));
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - rejects invalid persisted Track state", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("Track keys must be nonzero")
    {
      createRawIntegerRow(temp.path(), "tracks_hot", 0, makeHotData());
      createRawIntegerRow(temp.path(), "tracks_cold", 0, makeColdData());
      requireCorruptLibrary(temp.path());
    }

    SECTION("Track payloads must be canonical")
    {
      auto const invalidHot = std::array{std::byte{0x42}};
      createRawIntegerRow(temp.path(), "tracks_hot", 1, invalidHot);
      createRawIntegerRow(temp.path(), "tracks_cold", 1, makeColdData());
      requireCorruptLibrary(temp.path());
    }

    SECTION("Track dictionary references must resolve")
    {
      createRawIntegerRow(temp.path(), "tracks_hot", 1, makeHotData(TrackHotHeader{.artistId = DictionaryId{1}}));
      createRawIntegerRow(temp.path(), "tracks_cold", 1, makeColdData());
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - requires a strict Track and manifest bijection",
            "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("Track is missing its manifest")
    {
      createRawTrackPair(temp.path(), TrackId{1}, "a.flac");
      requireCorruptLibrary(temp.path());
    }

    SECTION("manifest names a missing Track")
    {
      createRawManifestRow(temp.path(), "a.flac", TrackId{1});
      requireCorruptLibrary(temp.path());
    }

    SECTION("manifest URI does not match its Track")
    {
      createRawTrackPair(temp.path(), TrackId{1}, "a.flac");
      createRawManifestRow(temp.path(), "b.flac", TrackId{1});
      requireCorruptLibrary(temp.path());
    }

    SECTION("manifest is bound to the wrong Track id")
    {
      createRawTrackPair(temp.path(), TrackId{1}, "a.flac");
      createRawManifestRow(temp.path(), "a.flac", TrackId{2});
      requireCorruptLibrary(temp.path());
    }

    SECTION("two Tracks participate in the same URI")
    {
      createRawTrackPair(temp.path(), TrackId{1}, "shared.flac");
      createRawTrackPair(temp.path(), TrackId{2}, "shared.flac");
      createRawManifestRow(temp.path(), "shared.flac", TrackId{1});
      createRawManifestRow(temp.path(), "extra.flac", TrackId{2});
      requireCorruptLibrary(temp.path());
    }

    SECTION("two manifests participate in the same Track id")
    {
      createRawTrackPair(temp.path(), TrackId{1}, "a.flac");
      createRawTrackPair(temp.path(), TrackId{2}, "b.flac");
      createRawManifestRow(temp.path(), "a.flac", TrackId{1});
      createRawManifestRow(temp.path(), "b.flac", TrackId{1});
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - rejects missing referenced Resources", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());
    createRawTrackPair(temp.path(), TrackId{1}, "covered.flac", ResourceId{1});
    createRawManifestRow(temp.path(), "covered.flac", TrackId{1});

    requireCorruptLibrary(temp.path());
  }

  TEST_CASE("MusicLibrary - rejects invalid persisted List state", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("List keys must be nonzero")
    {
      auto const validPayload = ao::test::requireValue(ListBuilder::makeEmpty().serialize());
      createRawIntegerRow(temp.path(), "lists", 0, validPayload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("List keys must have the native 32-bit width")
    {
      auto const shortKey = std::array{std::byte{1}, std::byte{0}};
      auto const validPayload = ao::test::requireValue(ListBuilder::makeEmpty().serialize());
      createRawIntegerKeyRow(temp.path(), "lists", shortKey, validPayload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("List payloads must have the exact canonical layout")
    {
      auto const invalidPayload = std::array{std::byte{0x42}};
      createRawIntegerRow(temp.path(), "lists", 1, invalidPayload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("saved-order Track ids must be nonzero")
    {
      auto builder = ListBuilder::makeEmpty();
      builder.orderTrackIds().add(TrackId{1});
      auto payload = ao::test::requireValue(builder.serialize());
      constexpr std::uint32_t kZeroTrackId = 0;
      std::memcpy(payload.data() + kListHeaderSize, &kZeroTrackId, sizeof(kZeroTrackId));
      createRawIntegerRow(temp.path(), "lists", 1, payload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("saved-order Track ids must be unique")
    {
      auto builder = ListBuilder::makeEmpty();
      builder.orderTrackIds().add(TrackId{1}).add(TrackId{2});
      auto payload = ao::test::requireValue(builder.serialize());
      constexpr std::uint32_t kDuplicateTrackId = 1;
      std::memcpy(payload.data() + kListHeaderSize + sizeof(TrackId), &kDuplicateTrackId, sizeof(kDuplicateTrackId));
      createRawIntegerRow(temp.path(), "lists", 1, payload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("parent must exist")
    {
      auto const payload = ao::test::requireValue(ListBuilder::makeEmpty().parentId(ListId{2}).serialize());
      createRawIntegerRow(temp.path(), "lists", 1, payload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("parent graph must be acyclic")
    {
      auto const first = ao::test::requireValue(ListBuilder::makeEmpty().parentId(ListId{2}).serialize());
      auto const second = ao::test::requireValue(ListBuilder::makeEmpty().parentId(ListId{1}).serialize());
      createRawIntegerRow(temp.path(), "lists", 1, first);
      createRawIntegerRow(temp.path(), "lists", 2, second);
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - accepts opaque invalid List filter text", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());
    constexpr auto kInvalidFilter = "((( not application grammar";
    auto const payload = ao::test::requireValue(ListBuilder::makeEmpty().filter(kInvalidFilter).serialize());
    createRawIntegerRow(temp.path(), "lists", 1, payload);

    auto libraryRes = openTestMusicLibrary(temp.path(), temp.path());

    REQUIRE(libraryRes);
    auto transaction = libraryRes->readTransaction();
    auto const optList = libraryRes->lists().reader(transaction).get(ListId{1});
    REQUIRE(optList);
    CHECK(optList->filter() == kInvalidFilter);
  }

  TEST_CASE("MusicLibrary - retains stale saved-order Track ids", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());
    auto builder = ListBuilder::makeEmpty();
    builder.orderTrackIds().add(TrackId{999});
    auto const payload = ao::test::requireValue(builder.serialize());
    createRawIntegerRow(temp.path(), "lists", 1, payload);

    auto library = makeTestMusicLibrary(temp.path(), temp.path());
    auto read = library.readTransaction();
    auto const optList = library.lists().reader(read).get(ListId{1});
    REQUIRE(optList);
    REQUIRE(optList->orderTrackIds().size() == 1);
    CHECK(optList->orderTrackIds()[0] == TrackId{999});
  }

  TEST_CASE("MusicLibrary - validates Resource rows while accepting opaque orphan data",
            "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("Resource key has the wrong width")
    {
      auto const shortKey = std::array{std::byte{1}, std::byte{0}};
      auto const payload = std::array{std::byte{0x42}};
      createRawIntegerKeyRow(temp.path(), "resources", shortKey, payload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("Resource id zero is reserved")
    {
      auto const payload = std::array{std::byte{0x42}};
      createRawIntegerRow(temp.path(), "resources", 0, payload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("Resource value must be exactly a descriptor")
    {
      createRawIntegerRow(temp.path(), "resources", 1, std::span<std::byte const>{});
      requireCorruptLibrary(temp.path());
    }

    SECTION("Resource value shorter than a descriptor is rejected")
    {
      auto const row = resourceDescriptorRow(1, 0);
      createRawIntegerRow(temp.path(), "resources", 1, std::span<std::byte const>{row}.first(row.size() - 1));
      requireCorruptLibrary(temp.path());
    }

    SECTION("Resource value longer than a descriptor is rejected")
    {
      auto wide = std::array<std::byte, kResourceDescriptorSize + 1>{};
      auto const row = resourceDescriptorRow(1, 0);
      std::ranges::copy(row, wide.begin());
      createRawIntegerRow(temp.path(), "resources", 1, wide);
      requireCorruptLibrary(temp.path());
    }

    SECTION("two rows carrying one digest are rejected")
    {
      auto const row = resourceDescriptorRow(7, 0);
      createRawIntegerRow(temp.path(), "resources", 7, row);
      createRawIntegerRow(temp.path(), "resources", 8, row);
      requireCorruptLibrary(temp.path());
    }

    SECTION("a row unreachable from its digest's initial key is rejected")
    {
      // A probe from 42 stops at the empty 43, so the row at 44 is invisible to
      // it and the next create for that digest would write a second row.
      createRawIntegerRow(temp.path(), "resources", 44, resourceDescriptorRow(42, 0));
      requireCorruptLibrary(temp.path());
    }

    SECTION("a long collision cluster is accepted")
    {
      constexpr std::uint32_t kHomeKey = 100;
      constexpr std::uint32_t kClusterLength = 16;

      for (std::uint32_t offset = 0; offset < kClusterLength; ++offset)
      {
        createRawIntegerRow(temp.path(),
                            "resources",
                            kHomeKey + offset,
                            resourceDescriptorRow(kHomeKey, static_cast<std::uint8_t>(offset)));
      }

      auto library = makeTestMusicLibrary(temp.path(), temp.path());
      auto read = library.readTransaction();
      CHECK(library.resources().reader(read).get(ResourceId{kHomeKey + kClusterLength - 1}));
    }

    SECTION("a cluster spanning the wrap to the first key is accepted")
    {
      constexpr auto kLastKey = std::numeric_limits<std::uint32_t>::max();
      createRawIntegerRow(temp.path(), "resources", kLastKey, resourceDescriptorRow(kLastKey, 0));
      createRawIntegerRow(temp.path(), "resources", 1, resourceDescriptorRow(kLastKey, 1));
      createRawIntegerRow(temp.path(), "resources", 2, resourceDescriptorRow(kLastKey, 2));

      auto library = makeTestMusicLibrary(temp.path(), temp.path());
      auto read = library.readTransaction();
      CHECK(library.resources().reader(read).get(ResourceId{2}));
    }

    SECTION("orphan Dictionary and unreferenced Resource rows are accepted")
    {
      // An unreferenced descriptor is the expected steady state of a rescanned
      // library, not a fault: rows are append-only and nothing sweeps them.
      auto const row = resourceDescriptorRow(1, 0);
      createRawIntegerRow(temp.path(), "dictionary", 1, createStringData("unused"));
      createRawIntegerRow(temp.path(), "resources", 1, row);

      auto library = makeTestMusicLibrary(temp.path(), temp.path());
      CHECK(library.dictionary().getOrDefault(DictionaryId{1}) == "unused");
      auto read = library.readTransaction();
      auto const optResource = library.resources().reader(read).get(ResourceId{1});
      REQUIRE(optResource);
      CHECK(deriveResourceId(optResource->digest) == ResourceId{1});
    }
  }

  TEST_CASE("MusicLibrary - rejects invalid persisted manifest state", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("manifest keys must be canonical library URIs")
    {
      auto const invalidKey = utility::bytes::view(std::string_view{"../x"});
      auto const validPayload = FileManifestBuilder::makeEmpty().trackId(TrackId{1}).serialize();
      createRawBlobRow(temp.path(), "file_manifest", invalidKey, validPayload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("manifest payloads must have the exact canonical layout")
    {
      auto const validKey = utility::bytes::view(std::string_view{"song"});
      auto const invalidPayload = std::array{std::byte{0x42}};
      createRawBlobRow(temp.path(), "file_manifest", validKey, invalidPayload);
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - accessors return valid references", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto const ml = makeTestMusicLibrary(temp.path(), temp.path());

    // All store accessors should be callable without crashing
    CHECK_NOTHROW(ml.tracks());
    CHECK_NOTHROW(ml.lists());
    CHECK_NOTHROW(ml.resources());
    CHECK_NOTHROW(ml.dictionary());
    CHECK_NOTHROW(ml.manifest());
    CHECK(ml.rootPath() == temp.path());

    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().tracks()), TrackStore const&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().lists()), ListStore const&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().resources()), ResourceStore const&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().dictionary()), DictionaryStore const&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().manifest()), FileManifestStore const&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().metadataHeader()), MetadataHeader>);
  }

  TEST_CASE("MusicLibrary - read and write transactions work", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = makeTestMusicLibrary(temp.path(), temp.path());

    auto wtxn = writeTransaction(ml);
    CHECK_NOTHROW(wtxn.commit());

    auto rtxn = ml.readTransaction(); // validates read access to the database
  }

  TEST_CASE("WritableMusicLibrary - excludes another writer session until release",
            "[library][unit][music-library][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");

    {
      auto firstWriterRes = WritableMusicLibrary::acquire(library);
      REQUIRE(firstWriterRes);

      auto secondWriterRes = WritableMusicLibrary::acquire(library);
      REQUIRE_FALSE(secondWriterRes);
      CHECK(secondWriterRes.error().code == Error::Code::Conflict);

      auto transaction = firstWriterRes->writeTransaction();
      REQUIRE(transaction.commit());
    }

    auto releasedWriterRes = WritableMusicLibrary::acquire(library);
    REQUIRE(releasedWriterRes);
  }

  TEST_CASE("WritableMusicLibrary - excludes a writer session in another process",
            "[library][integration][music-library][concurrency]")
  {
    constexpr auto kTimeout = std::chrono::seconds{15};
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto writableRes = WritableMusicLibrary::acquire(library);
    REQUIRE(writableRes);
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_probe");
    REQUIRE_FALSE(executablePath.empty());
    auto const scenario = std::string{"writer-conflict:"} + temp.path().filename().string();
    auto const result = ao::test::runProbeProcess(executablePath, scenario, kTimeout);

    REQUIRE(result.hasSuccessfulExit());
    CHECK(result.standardOutput == "writer-conflict");
    CHECK(result.standardError.empty());
  }

  TEST_CASE("WritableMusicLibrary - active transaction retains the writer session",
            "[library][unit][music-library][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto optTransaction = std::optional<WriteTransaction>{};

    {
      auto writerRes = WritableMusicLibrary::acquire(library);
      REQUIRE(writerRes);
      optTransaction.emplace(writerRes->writeTransaction());
    }

    auto activeTransactionWriterRes = WritableMusicLibrary::acquire(library);
    REQUIRE_FALSE(activeTransactionWriterRes);
    CHECK(activeTransactionWriterRes.error().code == Error::Code::Conflict);

    REQUIRE(optTransaction->commit());
    auto committedTransactionWriterRes = WritableMusicLibrary::acquire(library);
    REQUIRE(committedTransactionWriterRes);
  }

  TEST_CASE("WritableMusicLibrary - terminal transaction paths release the retained writer session",
            "[library][unit][music-library][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");

    SECTION("abort by destruction")
    {
      {
        auto writerRes = WritableMusicLibrary::acquire(library);
        REQUIRE(writerRes);
        auto transaction = writerRes->writeTransaction();
      }

      REQUIRE(WritableMusicLibrary::acquire(library));
    }

    SECTION("explicit abort")
    {
      auto optTransaction = std::optional<WriteTransaction>{};

      {
        auto writerRes = WritableMusicLibrary::acquire(library);
        REQUIRE(writerRes);
        optTransaction.emplace(writerRes->writeTransaction());
      }

      optTransaction->abort();
      REQUIRE(WritableMusicLibrary::acquire(library));
    }

    SECTION("commit failure")
    {
      auto optTransaction = std::optional<WriteTransaction>{};

      {
        auto writerRes = WritableMusicLibrary::acquire(library);
        REQUIRE(writerRes);
        optTransaction.emplace(writerRes->writeTransaction(WriteTransaction::Options{
          .optInjectedCommitFailure = Error{.code = Error::Code::IoError, .message = "injected failure"},
        }));
      }

      auto commitRes = optTransaction->commit();
      REQUIRE_FALSE(commitRes);
      CHECK(commitRes.error().code == Error::Code::IoError);
      REQUIRE(WritableMusicLibrary::acquire(library));
    }

    SECTION("storage mutation failure unwinds and rolls back")
    {
      constexpr std::size_t kMapSize = std::size_t{256} * 1024;
      auto smallLibrary = ao::test::requireValue(
        MusicLibrary::open(temp.path(), temp.path() / "small-db", MusicLibrary::Options{.pinnedMapBytes = kMapSize}));
      {
        auto writerRes = WritableMusicLibrary::acquire(smallLibrary);
        REQUIRE(writerRes);
        auto transaction = writerRes->writeTransaction();
        // A resource row is a fixed 36 bytes now, so a dictionary entry is what
        // still carries enough content to exhaust the whole map in one write.
        auto const oversizedText = std::string(kMapSize * 4, 'x');
        auto failureRes = transaction.apply(
          [&transaction, &oversizedText](LibraryWrite& /*write*/) -> Result<>
          {
            auto idRes = physicalDictionary(transaction).intern(oversizedText);

            if (!idRes)
            {
              return std::unexpected{idRes.error()};
            }

            return {};
          });
        REQUIRE_FALSE(failureRes);
        // A pinned map admits no growth, so a value larger than the whole map
        // exhausts it, and that arrives as capacity rather than as plain IO.
        CHECK(failureRes.error().code == Error::Code::StorageFull);
      }

      REQUIRE(WritableMusicLibrary::acquire(smallLibrary));
    }
  }
} // namespace ao::library::test
