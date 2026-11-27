// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestStore.h>

#include "lib/library/FileManifestValidation.h"
#include "test/TestFixtureSupport.h"
#include "test/library/LibraryStoreTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileStatus.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

  TEST_CASE("FileManifestStore - writes and reads back manifests")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);

    auto const signature = utility::xxh3Hash128("stored payload");
    constexpr auto kMtime = FileTimestamp{.seconds = 1719835200, .nanoseconds = 123456789};
    auto builder = FileManifestBuilder::makeEmpty();
    builder.trackId(TrackId{42})
      .fileSize(12345)
      .mtime(kMtime)
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
    CHECK(optView->mtime() == kMtime);
    CHECK(optView->audioPayloadLength() == 55555);
    CHECK(optView->audioSignature() == signature);
    CHECK(optView->status() == FileStatus::Available);
    CHECK(std::ranges::equal(optView->rawData(), payload));
  }

  TEST_CASE("FileManifestStore - preserves mtime presence and range across a durable roundtrip")
  {
    struct RoundtripManifest final
    {
      std::string_view uri;
      std::optional<FileTimestamp> optMtime{};
    };

    auto const manifests = std::array{
      RoundtripManifest{.uri = "epoch-zero.flac", .optMtime = FileTimestamp{.seconds = 0, .nanoseconds = 0}},
      RoundtripManifest{.uri = "absent.flac"},
      RoundtripManifest{
        .uri = "negative.flac", .optMtime = FileTimestamp{.seconds = -11644473600, .nanoseconds = 123456789}},
      RoundtripManifest{
        .uri = "wide.flac", .optMtime = FileTimestamp{.seconds = 987654321098765, .nanoseconds = 999999999}},
    };

    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);

    {
      auto writer = physicalWriter(store, wtxn);

      for (auto const& manifest : manifests)
      {
        auto builder = FileManifestBuilder::makeEmpty().mtime(manifest.optMtime);
        auto const prepared = ao::test::requireValue(builder.validate(manifest.uri)).bind(TrackId{42});
        REQUIRE(writer.put(prepared));
      }
    }

    REQUIRE(wtxn.commit());

    {
      auto rtxn = library.readTransaction();
      auto const reader = store.reader(rtxn);

      for (auto const& manifest : manifests)
      {
        CAPTURE(manifest.uri);
        auto const optView = reader.get(manifest.uri);
        REQUIRE(optView);

        if (auto const optMtime = optView->mtime(); manifest.optMtime)
        {
          REQUIRE(optMtime);
          CHECK(*optMtime == *manifest.optMtime);
        }
        else
        {
          CHECK_FALSE(optMtime);
        }
      }
    }
  }

  TEST_CASE("FileManifestStore - URI padding boundaries preserve read, write, and remove behavior")
  {
    // The longest key is within two bytes of LMDB's computed maximum key size
    // for 4 KiB pages, so a host whose limit is smaller fails its write.
    constexpr auto kUriLengths = std::array<std::size_t, 13>{1, 2, 3, 4, 5, 6, 7, 8, 9, 1'973, 1'974, 1'975, 1'976};

    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);

    auto builder = FileManifestBuilder::makeEmpty();
    builder.fileSize(12345).mtime(FileTimestamp{.seconds = 1719835200}).status(FileStatus::Available);
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
        REQUIRE(writer.tryRemove(uri));
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

  TEST_CASE("FileManifestStore - get returns nullopt for missing URI")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();

    auto const rtxn = library.readTransaction();
    CHECK_FALSE(store.reader(rtxn).get("nonexistent.flac"));
  }

  TEST_CASE("FileManifestStore::Reader - lowerBound seeks by canonical URI order")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);
    auto writer = physicalWriter(store, wtxn);
    auto builder = FileManifestBuilder::makeEmpty();
    builder.fileSize(123).mtime(FileTimestamp{.seconds = 1719835200}).status(FileStatus::Available);

    for (auto const uri : std::array<std::string_view, 3>{"abcde", "a", "abcd"})
    {
      auto const prepared = ao::test::requireValue(builder.validate(uri)).bind(TrackId{42});
      REQUIRE(writer.put(prepared));
    }

    REQUIRE(wtxn.commit());
    auto const rtxn = library.readTransaction();
    auto const reader = store.reader(rtxn);

    SECTION("exact URI")
    {
      auto it = reader.lowerBound("abcd");
      REQUIRE(it != reader.end());
      CHECK((*it).first == "abcd");
      ++it;
      REQUIRE(it != reader.end());
      CHECK((*it).first == "abcde");
      ++it;
      CHECK(it == reader.end());
    }

    SECTION("URI between stored keys")
    {
      auto const it = reader.lowerBound("ab");
      REQUIRE(it != reader.end());
      CHECK((*it).first == "abcd");
    }

    SECTION("URI after the final key")
    {
      CHECK(reader.lowerBound("z") == reader.end());
    }
  }

  TEST_CASE("FileManifestStore - remove is idempotent")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.manifest();
    auto wtxn = writeTransaction(library);

    auto builder = FileManifestBuilder::makeEmpty();
    builder.fileSize(12345).mtime(FileTimestamp{.seconds = 1719835200}).status(FileStatus::Available);
    auto const prepared = ao::test::requireValue(builder.validate("song.flac")).bind(TrackId{42});

    auto writer = physicalWriter(store, wtxn);
    REQUIRE(writer.put(prepared));
    CHECK(writer.tryRemove("song.flac"));
    CHECK_FALSE(writer.tryRemove("song.flac"));
    REQUIRE(wtxn.commit());

    auto rtxn = library.readTransaction();
    auto const optView = store.reader(rtxn).get("song.flac");
    CHECK_FALSE(optView);
  }

  TEST_CASE("FileManifestPayload - rejects malformed membership, size, and modification times")
  {
    auto const serialize = [](FileManifestHeader const& header)
    {
      auto bytes = std::vector<std::byte>(sizeof(header));
      std::memcpy(bytes.data(), &header, sizeof(header));
      return bytes;
    };
    auto validHeader = FileManifestHeader{.trackId = TrackId{1}};
    auto truncatedCarrier = validHeader;
    truncatedCarrier.trackId = kInvalidTrackId;
    truncatedCarrier.segmentCount = 1;
    auto zeroTrack = validHeader;
    zeroTrack.trackId = kInvalidTrackId;
    auto invalidMtimeFlag = validHeader;
    invalidMtimeFlag.hasMtime = 2;
    auto noncanonicalAbsentSeconds = validHeader;
    noncanonicalAbsentSeconds.mtimeSecondsLo = 1;
    auto noncanonicalAbsentNanoseconds = validHeader;
    noncanonicalAbsentNanoseconds.mtimeNanoseconds = 1;
    auto malformedMtimeNanoseconds = validHeader;
    malformedMtimeNanoseconds.mtime(FileTimestamp{.seconds = 1, .nanoseconds = 1000000000});

    struct InvalidPayloadCase final
    {
      std::string name;
      std::vector<std::byte> payload;
    };

    auto const invalidPayloads = std::array{
      InvalidPayloadCase{.name = "short", .payload = std::vector<std::byte>(sizeof(FileManifestHeader) - 1)},
      InvalidPayloadCase{.name = "long", .payload = std::vector<std::byte>(sizeof(FileManifestHeader) + 1)},
      InvalidPayloadCase{.name = "missing carrier tail", .payload = serialize(truncatedCarrier)},
      InvalidPayloadCase{.name = "zero whole-file Track id", .payload = serialize(zeroTrack)},
      InvalidPayloadCase{.name = "mtime-flag", .payload = serialize(invalidMtimeFlag)},
      InvalidPayloadCase{.name = "mtime-absent-seconds", .payload = serialize(noncanonicalAbsentSeconds)},
      InvalidPayloadCase{.name = "mtime-absent-nanoseconds", .payload = serialize(noncanonicalAbsentNanoseconds)},
      InvalidPayloadCase{.name = "mtime-nanoseconds", .payload = serialize(malformedMtimeNanoseconds)},
    };

    for (auto const& invalid : invalidPayloads)
    {
      CAPTURE(invalid.name);
      auto const validationRes = validateFileManifestPayload(invalid.payload);
      REQUIRE_FALSE(validationRes);
      CHECK(validationRes.error().code == Error::Code::CorruptData);
    }
  }
} // namespace ao::library::test
