// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestBuilder.h>

#include "lib/library/FileManifestValidation.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestView.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace ao::library::test
{
  TEST_CASE("FileManifestBuilder - constructs valid payload", "[library][unit][manifest]")
  {
    auto const signature = utility::xxh3Hash128("audio payload");
    auto builder = FileManifestBuilder::makeEmpty();
    builder.trackId(TrackId{100})
      .fileSize(999999)
      .mtime(888888)
      .audioPayloadLength(777777)
      .audioSignature(signature)
      .status(FileStatus::Error);

    auto payload = builder.serialize();

    // Validate via view
    auto view = FileManifestView{payload};

    CHECK(view.trackId() == ao::TrackId{100});
    CHECK(view.fileSize() == 999999);
    CHECK(view.mtime() == 888888);
    CHECK(view.audioPayloadLength() == 777777);
    CHECK(view.audioSignature() == signature);
    CHECK(view.status() == FileStatus::Error);
  }

  TEST_CASE("FileManifestBuilder - constructs from view", "[library][unit][manifest]")
  {
    auto const signature = utility::xxh3Hash128("copied payload");
    auto builder1 = FileManifestBuilder::makeEmpty();
    builder1.trackId(TrackId{123})
      .fileSize(111)
      .mtime(222)
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
    CHECK(view2.mtime() == 222);
    CHECK(view2.audioPayloadLength() == 444);
    CHECK(view2.audioSignature() == signature);
    CHECK(view2.status() == FileStatus::Missing);
  }

  TEST_CASE("FileManifestBuilder - unbound value owns canonical URI and payload snapshots", "[library][unit][manifest]")
  {
    auto uri = std::string{"snapshot.flac"};
    auto builder = FileManifestBuilder::makeEmpty();
    builder.trackId(TrackId{7}).fileSize(11).mtime(13).status(FileStatus::Missing);
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
    CHECK(view.mtime() == 13);
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
