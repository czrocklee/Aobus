// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestStore.h>

#include "test/unit/library/LibraryStoreTestSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    void seedPostOpenMalformedManifest(std::filesystem::path const& databasePath)
    {
      auto environmentResult = lmdb::Environment::open(
        databasePath.string(), {.flags = lmdb::kEnvNoTls, .maxDatabases = 8, .mapSize = kTestMusicLibraryMapSize});
      REQUIRE(environmentResult);
      auto environment = std::move(*environmentResult);
      auto transactionResult = lmdb::WriteTransaction::begin(environment);
      REQUIRE(transactionResult);
      auto transaction = std::move(*transactionResult);
      auto manifestResult = lmdb::Database::open(transaction, "file_manifest", lmdb::Database::KeyKind::Blob);
      REQUIRE(manifestResult);
      auto const malformedKey = utility::bytes::view(std::string_view{"zz"});
      auto const payload = FileManifestBuilder::makeEmpty().trackId(TrackId{1}).serialize();
      REQUIRE(manifestResult->writer(transaction).create(malformedKey, payload));
      REQUIRE(transaction.commit());
    }
  } // namespace

  TEST_CASE("FileManifestStore - create returns ValueTooLarge for invalid URI length", "[library][unit][manifest]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);
    auto writer = store.writer(wtxn);

    auto const longUri = std::string(501, 'a');

    auto const result = writer.put(longUri, std::span<std::byte const>{});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("FileManifestStore - writes and reads back manifests", "[library][unit][manifest]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);

    auto const signature = utility::xxh3Hash128("stored payload");
    auto builder = FileManifestBuilder::makeEmpty();
    builder.trackId(TrackId{42})
      .fileSize(12345)
      .mtime(67890)
      .audioPayloadLength(55555)
      .audioSignature(signature)
      .status(FileStatus::Available);
    auto const payload = builder.serialize();

    REQUIRE(store.writer(wtxn).put("song.flac", payload));
    REQUIRE(wtxn.commit());

    auto rtxn = library.readTransaction();
    auto const viewResult = store.reader(rtxn).get("song.flac");
    REQUIRE(viewResult);
    CHECK(viewResult->trackId() == TrackId{42});
    CHECK(viewResult->fileSize() == 12345);
    CHECK(viewResult->mtime() == 67890);
    CHECK(viewResult->audioPayloadLength() == 55555);
    CHECK(viewResult->audioSignature() == signature);
    CHECK(viewResult->status() == FileStatus::Available);
    CHECK(std::ranges::equal(viewResult->rawData(), payload));
  }

  TEST_CASE("FileManifestStore - rejects non-canonical or root-escaping URI keys", "[library][unit][manifest]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto transaction = writeTransaction(library);
    auto writer = library.manifest().writer(transaction);

    for (auto const uri : {std::string_view{},
                           std::string_view{"/absolute.flac"},
                           std::string_view{"../outside.flac"},
                           std::string_view{"album/../song.flac"},
                           std::string_view{R"(album\song.flac)"}})
    {
      CAPTURE(uri);
      auto const putResult = writer.put(uri, std::span<std::byte const>{});
      REQUIRE_FALSE(putResult);
      CHECK(putResult.error().code == Error::Code::InvalidInput);

      auto const getResult = writer.get(uri);
      REQUIRE_FALSE(getResult);
      CHECK(getResult.error().code == Error::Code::InvalidInput);

      auto const removeResult = writer.remove(uri);
      REQUIRE_FALSE(removeResult);
      CHECK(removeResult.error().code == Error::Code::InvalidInput);
    }
  }

  TEST_CASE("FileManifestStore - URI padding boundaries preserve read, write, and remove behavior",
            "[library][unit][manifest]")
  {
    constexpr auto kUriLengths = std::array<std::size_t, 13>{1, 2, 3, 4, 5, 6, 7, 8, 9, 497, 498, 499, 500};

    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);

    auto builder = FileManifestBuilder::makeEmpty();
    builder.trackId(TrackId{42}).fileSize(12345).mtime(67890).status(FileStatus::Available);
    auto const payload = builder.serialize();

    {
      auto writer = store.writer(wtxn);

      for (auto const uriLength : kUriLengths)
      {
        auto const uri = std::string(uriLength, 'a');
        REQUIRE(writer.put(uri, payload));

        auto const viewResult = writer.get(uri);
        REQUIRE(viewResult);
        CHECK(viewResult->trackId() == TrackId{42});
      }
    }

    REQUIRE(wtxn.commit());

    {
      auto const rtxn = library.readTransaction();
      auto const reader = store.reader(rtxn);

      for (auto const uriLength : kUriLengths)
      {
        auto const viewResult = reader.get(std::string(uriLength, 'a'));
        REQUIRE(viewResult);
        CHECK(viewResult->trackId() == TrackId{42});
      }
    }

    {
      auto removeTxn = writeTransaction(library);
      auto writer = store.writer(removeTxn);

      for (auto const uriLength : kUriLengths)
      {
        auto const uri = std::string(uriLength, 'a');
        REQUIRE(writer.remove(uri));

        auto const viewResult = writer.get(uri);
        REQUIRE_FALSE(viewResult);
        CHECK(viewResult.error().code == Error::Code::NotFound);
      }

      REQUIRE(removeTxn.commit());
    }

    auto const rtxn = library.readTransaction();
    auto const reader = store.reader(rtxn);

    for (auto const uriLength : kUriLengths)
    {
      auto const viewResult = reader.get(std::string(uriLength, 'a'));
      REQUIRE_FALSE(viewResult);
      CHECK(viewResult.error().code == Error::Code::NotFound);
    }
  }

  TEST_CASE("FileManifestStore - get returns NotFound for missing URI", "[library][unit][manifest]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();

    auto rtxn = library.readTransaction();
    auto const viewResult = store.reader(rtxn).get("nonexistent.flac");
    REQUIRE_FALSE(viewResult);
    CHECK(viewResult.error().code == Error::Code::NotFound);
  }

  TEST_CASE("FileManifestStore - remove is idempotent", "[library][unit][manifest]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);

    auto builder = FileManifestBuilder::makeEmpty();
    builder.trackId(TrackId{42}).fileSize(12345).mtime(67890).status(FileStatus::Available);

    auto writer = store.writer(wtxn);
    REQUIRE(writer.put("song.flac", builder.serialize()));
    REQUIRE(writer.remove("song.flac"));
    REQUIRE(writer.remove("song.flac"));
    REQUIRE(wtxn.commit());

    auto rtxn = library.readTransaction();
    auto const viewResult = store.reader(rtxn).get("song.flac");
    REQUIRE_FALSE(viewResult);
    CHECK(viewResult.error().code == Error::Code::NotFound);
  }

  TEST_CASE("FileManifestStore - rejects corrupt payloads before mutation", "[library][unit][manifest]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);
    auto invalidPayload = std::vector{std::byte{0x42}};

    auto const putResult = store.writer(wtxn).put("song.flac", invalidPayload);
    REQUIRE_FALSE(putResult);
    CHECK(putResult.error().code == Error::Code::CorruptData);
    REQUIRE(wtxn.commit());

    auto rtxn = library.readTransaction();
    auto const result = store.reader(rtxn).get("song.flac");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
  }

  TEST_CASE("FileManifestStore - post-open corrupt iterator rows fail fast",
            "[library][regression][manifest][integrity]")
  {
    auto fixture = LibraryStoreFixture{};
    seedPostOpenMalformedManifest(fixture.temp.path() / "db");
    auto transaction = fixture.library.readTransaction();
    auto iterator = fixture.library.manifest().reader(transaction).begin();

    CHECK_THROWS_WITH(std::ignore = *iterator,
                      "File manifest iterator encountered invalid data after library validation: File manifest key has "
                      "an invalid size");
  }

  TEST_CASE("FileManifestStore - exact payload validation is item-relative atomic",
            "[library][unit][manifest][integrity]")
  {
    auto const serialize = [](FileManifestHeader const& header)
    {
      auto bytes = std::vector<std::byte>(sizeof(header));
      std::memcpy(bytes.data(), &header, sizeof(header));
      return bytes;
    };
    auto validHeader = FileManifestHeader{.trackId = TrackId{1}};
    auto invalidStatus = validHeader;
    invalidStatus.status = static_cast<FileStatus>(0xff);
    auto nonzeroPadding = validHeader;
    nonzeroPadding.padding[1] = std::byte{1};
    auto lengthWithoutSignature = validHeader;
    lengthWithoutSignature.audioPayloadLength(1);
    auto signatureWithoutLength = validHeader;
    signatureWithoutLength.audioSignatureBytes[0] = std::byte{1};

    struct InvalidCase final
    {
      std::string name;
      std::vector<std::byte> payload;
    };

    auto const cases = std::array{
      InvalidCase{.name = "short", .payload = std::vector<std::byte>(sizeof(FileManifestHeader) - 1)},
      InvalidCase{.name = "long", .payload = std::vector<std::byte>(sizeof(FileManifestHeader) + 1)},
      InvalidCase{.name = "zero-track", .payload = FileManifestBuilder::makeEmpty().serialize()},
      InvalidCase{.name = "status", .payload = serialize(invalidStatus)},
      InvalidCase{.name = "padding", .payload = serialize(nonzeroPadding)},
      InvalidCase{.name = "length", .payload = serialize(lengthWithoutSignature)},
      InvalidCase{.name = "signature", .payload = serialize(signatureWithoutLength)},
    };

    auto fixture = LibraryStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto writer = fixture.library.manifest().writer(transaction);

    for (auto const& invalid : cases)
    {
      auto const uri = invalid.name + ".flac";
      CAPTURE(invalid.name);
      auto const putResult = writer.put(uri, invalid.payload);
      REQUIRE_FALSE(putResult);
      CHECK(putResult.error().code == Error::Code::CorruptData);
      auto const stagedRead = writer.get(uri);
      REQUIRE_FALSE(stagedRead);
      CHECK(stagedRead.error().code == Error::Code::NotFound);
    }

    REQUIRE(transaction.commit());
    auto readTransaction = fixture.library.readTransaction();
    auto reader = fixture.library.manifest().reader(readTransaction);

    for (auto const& invalid : cases)
    {
      auto const result = reader.get(invalid.name + ".flac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }
  }
} // namespace ao::library::test
