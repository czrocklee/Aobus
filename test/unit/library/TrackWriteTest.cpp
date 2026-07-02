// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/library/TrackBuilderTestSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>

#include <catch2/catch_test_macros.hpp>

#include <utility>

namespace ao::library::test
{
  namespace
  {
    std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold> prepareTrack(TrackBuilder& builder,
                                                                                  TrackSerializationContext& context)
    {
      auto result = builder.prepare(context.txn(), context.dict(), context.resources());
      REQUIRE(result);
      return *std::move(result);
    }
  } // namespace

  TEST_CASE("createPreparedTrackData writes prepared hot and cold track data", "[library][unit][track]")
  {
    auto context = TrackSerializationContext{};
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Created Track").artist("Artist");
    builder.property().uri("/tmp/created.flac");

    auto const [preparedHot, preparedCold] = prepareTrack(builder, context);

    auto fixture = TrackStoreFixture{};
    auto txn = beginWriteTransaction(fixture.env);
    auto writer = fixture.store.writer(txn);

    auto createResult = createPreparedTrackData(writer, preparedHot, preparedCold);
    REQUIRE(createResult);

    auto const& [trackId, view] = *createResult;
    CHECK(trackId != kInvalidTrackId);
    CHECK(view.metadata().title() == "Created Track");
    CHECK(view.property().uri() == "/tmp/created.flac");
  }

  TEST_CASE("updatePreparedTrackData replaces existing hot and cold track data", "[library][unit][track]")
  {
    auto context = TrackSerializationContext{};
    auto originalBuilder = TrackBuilder::createNew();
    originalBuilder.metadata().title("Original Track");
    originalBuilder.property().uri("/tmp/original.flac");
    auto const [originalHot, originalCold] = prepareTrack(originalBuilder, context);

    auto updatedBuilder = TrackBuilder::createNew();
    updatedBuilder.metadata().title("Updated Track");
    updatedBuilder.property().uri("/tmp/updated.flac");
    auto const [updatedHot, updatedCold] = prepareTrack(updatedBuilder, context);

    auto fixture = TrackStoreFixture{};
    auto txn = beginWriteTransaction(fixture.env);
    auto writer = fixture.store.writer(txn);
    auto createResult = createPreparedTrackData(writer, originalHot, originalCold);
    REQUIRE(createResult);

    auto const trackId = createResult->first;
    auto updateResult = updatePreparedTrackData(writer, trackId, updatedHot, updatedCold);
    REQUIRE(updateResult);

    auto optView = writer.get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView.has_value());
    CHECK(optView->metadata().title() == "Updated Track");
    CHECK(optView->property().uri() == "/tmp/updated.flac");
  }
} // namespace ao::library::test
