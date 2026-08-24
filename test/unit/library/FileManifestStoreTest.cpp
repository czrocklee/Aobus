// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestStore.h>

#include "lib/library/FileManifestValidation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/LibraryStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
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
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    template<typename Writer>
    concept HasRawManifestPut =
      requires(Writer& writer) { writer.put(std::string_view{}, std::span<std::byte const>{}); };

    static_assert(!HasRawManifestPut<FileManifestStore::Writer>);
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
    auto const prepared = ao::test::requireValue(builder.validate("song.flac")).bind(TrackId{42});

    REQUIRE(physicalWriter(store, wtxn).put(prepared));
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
    builder.fileSize(12345).mtime(67890).status(FileStatus::Available);
    {
      auto writer = physicalWriter(store, wtxn);

      for (auto const uriLength : kUriLengths)
      {
        auto const uri = std::string(uriLength, 'a');
        auto const prepared = ao::test::requireValue(builder.validate(uri)).bind(TrackId{42});
        REQUIRE(writer.put(prepared));

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
      auto writer = physicalWriter(store, removeTxn);

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

  TEST_CASE("FileManifestStore::Reader - lowerBound seeks by canonical URI order", "[library][unit][manifest]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);
    auto writer = physicalWriter(store, wtxn);
    auto builder = FileManifestBuilder::makeEmpty().fileSize(123).mtime(456).status(FileStatus::Available);

    for (auto const uri : std::array<std::string_view, 3>{"alpha.flac", "delta.flac", "omega.flac"})
    {
      auto const prepared = ao::test::requireValue(builder.validate(uri)).bind(TrackId{42});
      REQUIRE(writer.put(prepared));
    }

    REQUIRE(wtxn.commit());
    auto const rtxn = library.readTransaction();
    auto const reader = store.reader(rtxn);

    SECTION("exact URI")
    {
      auto const it = reader.lowerBound("delta.flac");
      REQUIRE(it != reader.end());
      CHECK((*it).first == "delta.flac");
    }

    SECTION("URI between stored keys")
    {
      auto const it = reader.lowerBound("echo.flac");
      REQUIRE(it != reader.end());
      CHECK((*it).first == "omega.flac");
    }

    SECTION("URI after the final key")
    {
      CHECK(reader.lowerBound("zulu.flac") == reader.end());
    }
  }

  TEST_CASE("FileManifestStore - remove is idempotent", "[library][unit][manifest]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);

    auto builder = FileManifestBuilder::makeEmpty();
    builder.fileSize(12345).mtime(67890).status(FileStatus::Available);
    auto const prepared = ao::test::requireValue(builder.validate("song.flac")).bind(TrackId{42});

    auto writer = physicalWriter(store, wtxn);
    REQUIRE(writer.put(prepared));
    CHECK(writer.remove("song.flac"));
    CHECK_FALSE(writer.remove("song.flac"));
    REQUIRE(wtxn.commit());

    auto rtxn = library.readTransaction();
    auto const optView = store.reader(rtxn).get("song.flac");
    CHECK_FALSE(optView);
  }

  TEST_CASE("FileManifestStore - failed preparation cannot mutate the Store", "[library][unit][manifest][integrity]")
  {
    auto const serialize = [](FileManifestHeader const& header)
    {
      auto bytes = std::vector<std::byte>(sizeof(header));
      std::memcpy(bytes.data(), &header, sizeof(header));
      return bytes;
    };
    auto validHeader = FileManifestHeader{.trackId = TrackId{1}};
    auto nonzeroPadding = validHeader;
    nonzeroPadding.padding[1] = std::byte{1};
    auto zeroTrack = validHeader;
    zeroTrack.trackId = kInvalidTrackId;

    struct InvalidPayloadCase final
    {
      std::string name;
      std::vector<std::byte> payload;
    };

    auto const invalidPayloads = std::array{
      InvalidPayloadCase{.name = "short", .payload = std::vector<std::byte>(sizeof(FileManifestHeader) - 1)},
      InvalidPayloadCase{.name = "long", .payload = std::vector<std::byte>(sizeof(FileManifestHeader) + 1)},
      InvalidPayloadCase{.name = "padding", .payload = serialize(nonzeroPadding)},
      InvalidPayloadCase{.name = "zero-track", .payload = serialize(zeroTrack)},
    };

    for (auto const& invalid : invalidPayloads)
    {
      CAPTURE(invalid.name);
      auto const validationRes = validateFileManifestPayload(invalid.payload);
      REQUIRE_FALSE(validationRes);
      CHECK(validationRes.error().code == Error::Code::CorruptData);
    }

    // A zero Track id is no longer a preparation fact: validation is independent
    // of the binding, and only the complete payload validator above rejects it.
    auto const invalidCandidates = std::array{
      std::pair{"status", FileManifestBuilder::makeEmpty().status(static_cast<FileStatus>(0xff))},
      std::pair{"length", FileManifestBuilder::makeEmpty().audioPayloadLength(1)},
      std::pair{"signature", FileManifestBuilder::makeEmpty().audioSignature(utility::xxh3Hash128("signature"))},
    };

    auto fixture = LibraryStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto writer = physicalWriter(fixture.library.manifest(), transaction);

    for (auto const& [name, builder] : invalidCandidates)
    {
      auto const uri = std::string{name} + ".flac";
      CAPTURE(name);
      auto const unboundRes = builder.validate(uri);
      REQUIRE_FALSE(unboundRes);
      CHECK(unboundRes.error().code == Error::Code::CorruptData);
      auto const optStagedRead = writer.get(uri);
      CHECK_FALSE(optStagedRead);
    }

    REQUIRE(transaction.commit());
    auto readTransaction = fixture.library.readTransaction();
    auto reader = fixture.library.manifest().reader(readTransaction);

    for (auto const& [name, builder] : invalidCandidates)
    {
      std::ignore = builder;
      auto const optResult = reader.get(std::string{name} + ".flac");
      CHECK_FALSE(optResult);
    }
  }
} // namespace ao::library::test
