// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/library/WriteTransaction.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace ao::library::test
{
  namespace
  {
    std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold> prepareTrack(TrackBuilder& builder,
                                                                                  WriteTransaction& transaction,
                                                                                  ResourceStore const& resources)
    {
      auto result = builder.prepare(transaction, resources);
      REQUIRE(result);
      return *std::move(result);
    }
  } // namespace

  TEST_CASE("createPreparedTrackRecord writes prepared hot and cold track records", "[library][unit][track]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Created Track").artist("Artist");
    builder.property().uri("/tmp/created.flac");

    auto const [preparedHot, preparedCold] = prepareTrack(builder, transaction, fixture.library.resources());
    auto writer = fixture.store.writer(transaction);

    auto createResult = createPreparedTrackRecord(writer, preparedHot, preparedCold);
    REQUIRE(createResult);

    auto const& [trackId, view] = *createResult;
    CHECK(trackId != kInvalidTrackId);
    CHECK(view.metadata().title() == "Created Track");
    CHECK(view.property().uri() == "/tmp/created.flac");
    REQUIRE(transaction.commit());
  }

  TEST_CASE("updatePreparedTrackRecord replaces existing hot and cold track records", "[library][unit][track]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto originalBuilder = TrackBuilder::makeEmpty();
    originalBuilder.metadata().title("Original Track");
    originalBuilder.property().uri("/tmp/original.flac");
    auto const [originalHot, originalCold] = prepareTrack(originalBuilder, transaction, fixture.library.resources());

    auto updatedBuilder = TrackBuilder::makeEmpty();
    updatedBuilder.metadata().title("Updated Track");
    updatedBuilder.property().uri("/tmp/updated.flac");
    auto const [updatedHot, updatedCold] = prepareTrack(updatedBuilder, transaction, fixture.library.resources());

    auto writer = fixture.store.writer(transaction);
    auto createResult = createPreparedTrackRecord(writer, originalHot, originalCold);
    REQUIRE(createResult);

    auto const trackId = createResult->first;
    auto updateResult = updatePreparedTrackRecord(writer, trackId, updatedHot, updatedCold);
    REQUIRE(updateResult);

    auto optView = writer.get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Updated Track");
    CHECK(optView->property().uri() == "/tmp/updated.flac");
    REQUIRE(transaction.commit());
  }

  TEST_CASE("prepared track data is a snapshot unaffected by later builder mutation", "[library][unit][track]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto builder = TrackBuilder::makeEmpty();

    {
      // Inputs the builder only borrows as string_views; they go out of
      // scope after prepare to prove the prepared value owns its bytes.
      auto const title = std::string{"Snapshot Title"};
      auto const uri = std::string{"/tmp/snapshot.flac"};
      builder.metadata().title(title).trackNumber(3);
      builder.property().uri(uri);

      auto const [preparedHot, preparedCold] = prepareTrack(builder, transaction, fixture.library.resources());

      auto const longerTitle = std::string{"Mutated Title That Is Much Longer Than Before"};
      auto const longerUri = std::string{"/tmp/mutated/path/that/is/much/longer.flac"};
      builder.metadata().title(longerTitle).trackNumber(9);
      builder.property().uri(longerUri);

      auto writer = fixture.store.writer(transaction);

      auto createResult = createPreparedTrackRecord(writer, preparedHot, preparedCold);
      REQUIRE(createResult);

      auto const& [trackId, view] = *createResult;
      CHECK(trackId != kInvalidTrackId);
      CHECK(view.isHotValid());
      CHECK(view.isColdValid());
      CHECK(view.metadata().title() == "Snapshot Title");
      CHECK(view.metadata().trackNumber() == 3);
      CHECK(view.property().uri() == "/tmp/snapshot.flac");
      REQUIRE(transaction.commit());
    }
  }
} // namespace ao::library::test
