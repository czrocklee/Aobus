// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestStore.h>

#include "test/unit/library/LibraryStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
  } // namespace

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
    auto const optView = store.reader(rtxn).get("song.flac");
    REQUIRE(optView);
    CHECK(optView->trackId() == TrackId{42});
    CHECK(optView->fileSize() == 12345);
    CHECK(optView->mtime() == 67890);
    CHECK(optView->audioPayloadLength() == 55555);
    CHECK(optView->audioSignature() == signature);
    CHECK(optView->status() == FileStatus::Available);
    CHECK(std::ranges::equal(optView->rawData(), payload));
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

        auto const optView = writer.get(uri);
        REQUIRE(optView);
        CHECK(optView->trackId() == TrackId{42});
      }
    }

    REQUIRE(wtxn.commit());

    {
      auto const rtxn = library.readTransaction();
      auto const reader = store.reader(rtxn);

      for (auto const uriLength : kUriLengths)
      {
        auto const optView = reader.get(std::string(uriLength, 'a'));
        REQUIRE(optView);
        CHECK(optView->trackId() == TrackId{42});
      }
    }

    {
      auto removeTxn = writeTransaction(library);
      auto writer = store.writer(removeTxn);

      for (auto const uriLength : kUriLengths)
      {
        auto const uri = std::string(uriLength, 'a');
        CAPTURE(uriLength);

        auto const optView = writer.get(uri);
        REQUIRE(optView);
        REQUIRE(writer.remove(uri));
        auto const optViewAgain = writer.get(uri);
        CHECK_FALSE(optViewAgain);
      }

      REQUIRE(removeTxn.commit());
    }

    {
      auto const rtxn = library.readTransaction();
      auto const reader = store.reader(rtxn);

      for (auto const uriLength : kUriLengths)
      {
        CAPTURE(uriLength);
        auto const optView = reader.get(std::string(uriLength, 'a'));
        CHECK_FALSE(optView);
      }
    }
  }

  TEST_CASE("FileManifestStore - get returns nullopt for missing URI", "[library][unit][manifest]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();

    SECTION("FileManifestStore::Reader returns nullopt for non-existent entry")
    {
      auto const rtxnObj = fixture.library.readTransaction();
      auto const optView = store.reader(rtxnObj).get("nonexistent.flac");
      CHECK_FALSE(optView);
    }
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
    CHECK(writer.remove("song.flac"));
    CHECK_FALSE(writer.remove("song.flac"));
    REQUIRE(wtxn.commit());

    auto rtxn = library.readTransaction();
    auto const optView = store.reader(rtxn).get("song.flac");
    CHECK_FALSE(optView);
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
      auto const optStagedRead = writer.get(uri);
      CHECK_FALSE(optStagedRead);
    }

    REQUIRE(transaction.commit());
    auto readTransaction = fixture.library.readTransaction();
    auto reader = fixture.library.manifest().reader(readTransaction);

    for (auto const& invalid : cases)
    {
      auto const optResult = reader.get(invalid.name + ".flac");
      CHECK_FALSE(optResult);
    }
  }
} // namespace ao::library::test
