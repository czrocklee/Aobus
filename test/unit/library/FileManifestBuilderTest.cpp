// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestBuilder.h>

#include "lib/library/FileManifestValidation.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/FileTimestamp.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestView.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace ao::library::test
{
  TEST_CASE("FileManifestBuilder - constructs valid payload", "[library][unit][manifest]")
  {
    auto const signature = utility::xxh3Hash128("audio payload");
    constexpr auto kMtime = FileTimestamp{.seconds = 1719835200, .nanoseconds = 123456789};
    auto builder = FileManifestBuilder::makeEmpty();
    builder.trackId(TrackId{100})
      .fileSize(999999)
      .mtime(kMtime)
      .audioPayloadLength(777777)
      .audioSignature(signature)
      .status(FileStatus::Error);

    auto payload = builder.serialize();

    // Validate via view
    auto view = FileManifestView{payload};

    CHECK(view.trackId() == ao::TrackId{100});
    CHECK(view.fileSize() == 999999);
    CHECK(view.mtime() == kMtime);
    CHECK(view.audioPayloadLength() == 777777);
    CHECK(view.audioSignature() == signature);
    CHECK(view.status() == FileStatus::Error);
  }

  TEST_CASE("FileManifestBuilder - constructs from view", "[library][unit][manifest]")
  {
    auto const signature = utility::xxh3Hash128("copied payload");
    constexpr auto kMtime = FileTimestamp{.seconds = 1719835200, .nanoseconds = 987654321};
    auto builder1 = FileManifestBuilder::makeEmpty();
    builder1.trackId(TrackId{123})
      .fileSize(111)
      .mtime(kMtime)
      .audioPayloadLength(444)
      .audioSignature(signature)
      .status(FileStatus::Missing);

    auto payload1 = builder1.serialize();
    auto view = FileManifestView{payload1};

    auto builder2 = FileManifestBuilder::fromView(view);
    builder2.fileSize(333); // Modify one field

    auto payload2 = builder2.serialize();
    auto view2 = FileManifestView{payload2};

    CHECK(view2.trackId() == ao::TrackId{123});
    CHECK(view2.fileSize() == 333);
    CHECK(view2.mtime() == kMtime);
    CHECK(view2.audioPayloadLength() == 444);
    CHECK(view2.audioSignature() == signature);
    CHECK(view2.status() == FileStatus::Missing);
  }

  TEST_CASE("FileManifestBuilder - stores epoch zero and absence as distinct canonical facts",
            "[library][unit][manifest]")
  {
    // Epoch zero is a present instant, not the canonical absent encoding.
    {
      auto const builder = FileManifestBuilder::makeEmpty().mtime(FileTimestamp{});
      auto unbound = ao::test::requireValue(builder.validate("epoch-zero.flac"));
      auto const prepared = std::move(unbound).bind(TrackId{3});

      REQUIRE(validateFileManifestPayload(prepared.bytes()));

      auto const optMtime = FileManifestView{prepared.bytes()}.mtime();
      REQUIRE(optMtime);
      CHECK(*optMtime == FileTimestamp{});
    }

    // A builder that never set an mtime round-trips canonical absence.
    {
      auto const builder = FileManifestBuilder::makeEmpty();
      auto unbound = ao::test::requireValue(builder.validate("absent.flac"));
      auto const prepared = std::move(unbound).bind(TrackId{3});

      REQUIRE(validateFileManifestPayload(prepared.bytes()));
      CHECK_FALSE(FileManifestView{prepared.bytes()}.mtime());
    }

    // Clearing a previously stored instant restores canonical absence.
    {
      auto const builder = FileManifestBuilder::makeEmpty().mtime(FileTimestamp{}).mtime(std::nullopt);
      auto unbound = ao::test::requireValue(builder.validate("cleared.flac"));
      auto const prepared = std::move(unbound).bind(TrackId{3});

      REQUIRE(validateFileManifestPayload(prepared.bytes()));
      CHECK_FALSE(FileManifestView{prepared.bytes()}.mtime());
    }
  }

  TEST_CASE("FileManifestBuilder - round-trips negative and wide instants exactly", "[library][unit][manifest]")
  {
    auto const roundTrip = [](FileTimestamp const timestamp)
    {
      auto const payload = FileManifestBuilder::makeEmpty().trackId(TrackId{3}).mtime(timestamp).serialize();

      REQUIRE(validateFileManifestPayload(payload));

      auto const optMtime = FileManifestView{payload}.mtime();
      REQUIRE(optMtime);
      CHECK(*optMtime == timestamp);
    };

    // 1601-01-01 UTC with a nonzero fraction, before the Unix epoch.
    roundTrip(FileTimestamp{.seconds = -11644473600, .nanoseconds = 999999999});
    // The widest representable instant keeps every second bit.
    roundTrip(FileTimestamp{.seconds = std::numeric_limits<std::int64_t>::max(), .nanoseconds = 999999999});
    // The most negative instant keeps its sign bits across the word split.
    roundTrip(FileTimestamp{.seconds = std::numeric_limits<std::int64_t>::min(), .nanoseconds = 999999999});
  }

  TEST_CASE("FileManifestBuilder - unbound value owns canonical URI and payload snapshots", "[library][unit][manifest]")
  {
    auto uri = std::string{"snapshot.flac"};
    constexpr auto kMtime = FileTimestamp{.seconds = 1719835200, .nanoseconds = 13};
    auto builder = FileManifestBuilder::makeEmpty();
    builder.trackId(TrackId{7}).fileSize(11).mtime(kMtime).status(FileStatus::Missing);
    auto unbound = ao::test::requireValue(builder.validate(uri));

    uri = "mutated.flac";
    builder.trackId(TrackId{9}).fileSize(17);
    auto const prepared = std::move(unbound).bind(TrackId{21});
    auto const bytes = prepared.bytes();
    auto const view = FileManifestView{bytes};

    CHECK(prepared.uri() == "snapshot.flac");
    REQUIRE(validateFileManifestPayload(bytes));
    // Neither builder id survives validation; only the bound id reaches storage.
    CHECK(view.trackId() == TrackId{21});
    CHECK(view.fileSize() == 11);
    CHECK(view.mtime() == kMtime);
    CHECK(view.status() == FileStatus::Missing);
  }

  TEST_CASE("FileManifestBuilder - validation is independent of the Track binding", "[library][unit][manifest]")
  {
    auto const builder = FileManifestBuilder::makeEmpty();
    auto unbound = ao::test::requireValue(builder.validate("unbound.flac"));

    CHECK(unbound.uri() == "unbound.flac");

    auto const prepared = std::move(unbound).bind(TrackId{5});

    REQUIRE(validateFileManifestPayload(prepared.bytes()));
    CHECK(FileManifestView{prepared.bytes()}.trackId() == TrackId{5});
  }

  TEST_CASE("FileManifestBuilder - validation rejects broken record facts", "[library][unit][manifest]")
  {
    auto const invalidStatusRes =
      FileManifestBuilder::makeEmpty().status(static_cast<FileStatus>(0xff)).validate("status.flac");

    REQUIRE_FALSE(invalidStatusRes);
    CHECK(invalidStatusRes.error().code == Error::Code::CorruptData);

    auto const pendingSignatureRes = FileManifestBuilder::makeEmpty().audioPayloadLength(1).validate("identity.flac");

    REQUIRE_FALSE(pendingSignatureRes);
    CHECK(pendingSignatureRes.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("FileManifestBuilder - validation rejects malformed mtime nanoseconds", "[library][unit][manifest]")
  {
    SECTION("the last valid fraction is accepted")
    {
      auto const validRes = FileManifestBuilder::makeEmpty()
                              .mtime(FileTimestamp{.seconds = 1, .nanoseconds = 999999999})
                              .validate("last-valid.flac");
      REQUIRE(validRes);
    }

    SECTION("a full second of nanoseconds is rejected")
    {
      auto const fullSecondRes = FileManifestBuilder::makeEmpty()
                                   .mtime(FileTimestamp{.seconds = 1, .nanoseconds = 1000000000})
                                   .validate("full-second.flac");
      REQUIRE_FALSE(fullSecondRes);
      CHECK(fullSecondRes.error().code == Error::Code::CorruptData);
    }

    SECTION("the widest uint32 fraction is rejected")
    {
      auto const wideRes = FileManifestBuilder::makeEmpty()
                             .mtime(FileTimestamp{.seconds = 1, .nanoseconds = 4294967295U})
                             .validate("widest.flac");
      REQUIRE_FALSE(wideRes);
      CHECK(wideRes.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("FileManifestBuilder - validation rejects a non-canonical URI", "[library][unit][manifest]")
  {
    auto const unboundRes = FileManifestBuilder::makeEmpty().validate("../outside.flac");

    REQUIRE_FALSE(unboundRes);
    CHECK(unboundRes.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("FileManifestBuilder - the complete payload validator still rejects Track zero",
            "[library][unit][manifest]")
  {
    auto const payload = FileManifestBuilder::makeEmpty().serialize();
    auto const validationRes = validateFileManifestPayload(payload);

    REQUIRE_FALSE(validationRes);
    CHECK(validationRes.error().code == Error::Code::CorruptData);
  }
} // namespace ao::library::test
