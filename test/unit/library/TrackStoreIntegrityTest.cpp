// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/library/TrackWrite.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::library::test
{
  // TrackStore only accepts prepared records, which serialize into storage-owned
  // bytes, so a non-canonical Track can no longer be offered to the writer. What
  // still has to hold is that such a record never survives being read back: the
  // canonical validators run again over persisted bytes at MusicLibrary::open().
  TEST_CASE("MusicLibrary - open rejects a non-canonical persisted hot Track record",
            "[library][regression][track-store][track-integrity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const hot = makeHotData(TrackHotHeader{.tagBloom = 1}, "Invalid bloom");

    initializeLibraryStorage(temp.path());
    seedRawTrackRow(temp.path(), 1, hot, makeColdData());
    requireCorruptOpen(temp.path());
  }

  TEST_CASE("MusicLibrary - open rejects a non-canonical persisted cold Track record",
            "[library][regression][track-store][track-integrity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const cold = makeColdData(TrackColdHeader{.reserved8 = 1});

    initializeLibraryStorage(temp.path());
    seedRawTrackRow(temp.path(), 1, makeHotData(), cold);
    requireCorruptOpen(temp.path());
  }

  TEST_CASE("MusicLibrary - open rejects a non-NFC persisted Track title",
            "[library][regression][track-store][unicode]")
  {
    auto const temp = ao::test::TempDir{};
    auto const hot = makeHotData({}, "Cafe\u0301");

    initializeLibraryStorage(temp.path());
    seedRawTrackRow(temp.path(), 1, hot, makeColdData());
    requireCorruptOpen(temp.path());
  }

  TEST_CASE("TrackStore - prepared updates of an absent Track are non-mutating NotFound",
            "[library][regression][track-store][track-integrity]")
  {
    auto fixture = TrackStoreFixture{};
    auto const original = makeTrackSpec("Original");
    auto originalBuilder = TrackBuilder::makeEmpty();
    applyTrackSpec(originalBuilder, original);
    auto const existingId = createCommittedTrack(fixture.library, originalBuilder);

    auto transaction = writeTransaction(fixture.library);
    auto const replacement = makeTrackSpec("Replacement");
    auto builder = TrackBuilder::makeEmpty();
    applyTrackSpec(builder, replacement);
    builder.property().uri("replacement.flac");
    auto preparedRes = physicalPrepareTrack(builder, transaction, fixture.library.resources());
    REQUIRE(preparedRes);

    auto writer = physicalWriter(fixture.library.tracks(), transaction);
    auto const missingId = TrackId{existingId.raw() + 1};
    enum class UpdateMode : std::uint8_t
    {
      Hot,
      Cold,
      Both,
    };
    auto mode = UpdateMode::Hot;

    SECTION("hot side")
    {
      mode = UpdateMode::Hot;
    }

    SECTION("cold side")
    {
      mode = UpdateMode::Cold;
    }

    SECTION("paired record")
    {
      mode = UpdateMode::Both;
    }

    auto const result = [&] -> Result<>
    {
      switch (mode)
      {
        case UpdateMode::Hot: return updatePreparedHotTrackRecord(writer, missingId, preparedRes->first);
        case UpdateMode::Cold: return updatePreparedColdTrackRecord(writer, missingId, preparedRes->second);
        case UpdateMode::Both:
          return updatePreparedTrackRecord(writer, missingId, preparedRes->first, preparedRes->second);
      }

      return makeError(Error::Code::InvalidState, "Unreachable update mode");
    }();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    REQUIRE(transaction.commit());

    auto readTransaction = fixture.library.readTransaction();
    auto reader = fixture.library.tracks().reader(readTransaction);
    auto optView = reader.get(existingId);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Original");
    CHECK_FALSE(reader.get(missingId, TrackStore::Reader::LoadMode::Hot));
    CHECK_FALSE(reader.get(missingId, TrackStore::Reader::LoadMode::Cold));
  }

  TEST_CASE("TrackStore - writer rejects the reserved Track id before probing storage",
            "[library][regression][track-store][track-integrity]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto const reserved = makeTrackSpec("Reserved");
    auto builder = TrackBuilder::makeEmpty();
    applyTrackSpec(builder, reserved);
    auto preparedRes = physicalPrepareHotTrack(builder, transaction);
    REQUIRE(preparedRes);

    auto writer = physicalWriter(fixture.library.tracks(), transaction);

    // Track zero is a corrupt target, not a recoverable miss, so it must not
    // read back as NotFound just because no row occupies key zero.
    auto const result = updatePreparedHotTrackRecord(writer, kInvalidTrackId, *preparedRes);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::CorruptData);
    REQUIRE(transaction.commit());
  }
} // namespace ao::library::test
