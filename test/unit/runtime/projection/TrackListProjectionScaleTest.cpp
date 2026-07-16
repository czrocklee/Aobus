// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackBuilder.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    template<typename State>
    concept OwnsMaterializedTrackIds = requires(State const& state) { state.trackIds; };

    static_assert(!OwnsMaterializedTrackIds<PlaybackSequenceState>);
  } // namespace

  TEST_CASE("TrackListProjection - 10k scale operations preserve indices and deltas",
            "[runtime][unit][projection][scale]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto& lib = libraryFixture.library();

    int const kTrackCount = 10000;
    auto addTracks = [&lib](std::int32_t count, auto&& makeSpec)
    {
      auto transaction = library::test::writeTransaction(lib);
      auto writer = lib.tracks().writer(transaction);
      auto result = std::vector<TrackId>{};
      result.reserve(static_cast<std::size_t>(count));

      for (std::int32_t index = 0; index < count; ++index)
      {
        auto builder = library::TrackBuilder::makeEmpty();
        auto const spec = makeSpec(index);
        library::test::applyTrackSpec(builder, spec);

        auto data = builder.serialize(transaction, lib.resources());
        REQUIRE(data);
        auto createResult = writer.createHotCold(data->first, data->second);
        REQUIRE(createResult);
        result.push_back(createResult->first);
      }

      REQUIRE(transaction.commit());
      return result;
    };

    auto ids = addTracks(kTrackCount,
                         [](std::int32_t index)
                         {
                           return library::test::TrackSpec{.title = std::format("Track {:05d}", index),
                                                           .artist = std::format("Artist {:03d}", index % 100),
                                                           .album = std::format("Album {:03d}", index % 500)};
                         });

    auto sourcePtr = std::make_shared<MutableTrackSource>();
    sourcePtr->setInitial(ids);

    auto proj = LiveTrackListProjection{
      kInvalidViewId,
      TrackSourceLease{sourcePtr},
      lib,
      TrackOrderSpec{.sortBy = {TrackSortTerm{.field = TrackSortField::Title}}},
    };

    CHECK(proj.size() == kTrackCount);
    CHECK(proj.viewId() == kInvalidViewId);

    SECTION("Batch insertion performance (incremental merge)")
    {
      auto newIds = addTracks(100,
                              [](std::int32_t index)
                              { return library::test::TrackSpec{.title = std::format("New Track {:05d}", index)}; });

      sourcePtr->batchInsert(newIds);

      CHECK(proj.size() == kTrackCount + 100);

      for (auto const id : newIds)
      {
        auto const optIndex = proj.indexOf(id);
        REQUIRE(optIndex);
        CHECK(proj.trackIdAt(*optIndex) == id);
      }
    }

    SECTION("indexOf returns stable positions across the full list")
    {
      auto const firstId = proj.trackIdAt(0);
      auto const midId = proj.trackIdAt(static_cast<std::size_t>(kTrackCount / 2));
      auto const lastId = proj.trackIdAt(static_cast<std::size_t>(kTrackCount - 1));

      for (std::int32_t i = 0; i < 10; ++i)
      {
        CHECK(proj.indexOf(firstId) == std::size_t{0});
        CHECK(proj.indexOf(midId) == static_cast<std::size_t>(kTrackCount / 2));
        CHECK(proj.indexOf(lastId) == static_cast<std::size_t>(kTrackCount - 1));
      }

      CHECK_FALSE(proj.indexOf(TrackId{999999}).has_value());
    }

    SECTION("detached projection applies representative remove and update batches")
    {
      auto batches = std::vector<TrackListProjectionDeltaBatch>{};
      auto const sub = proj.subscribe([&](TrackListProjectionDeltaBatch const& batch) { batches.push_back(batch); });
      batches.clear(); // Ignore the subscription's initial Reset snapshot.

      auto const removedIds = std::array{ids[100], ids[5000], ids[9999]};
      sourcePtr->batchRemove(removedIds);

      REQUIRE(batches.size() == 1);
      CHECK(proj.size() == static_cast<std::size_t>(kTrackCount) - removedIds.size());

      for (auto const trackId : removedIds)
      {
        CHECK_FALSE(proj.indexOf(trackId));
      }

      batches.clear();
      auto const updatedIds = std::array{ids[10], ids[9000]};
      sourcePtr->batchUpdate(updatedIds);

      REQUIRE(batches.size() == 1);
      CHECK(std::ranges::all_of(batches.front().deltas,
                                [](TrackListProjectionDelta const& delta)
                                { return std::holds_alternative<ProjectionUpdateRange>(delta); }));
    }

    SECTION("reset delta keeps projected size stable")
    {
      auto const projSize = proj.size();
      bool receivedReset = false;
      std::size_t receivedSize = 0;

      auto const sub = proj.subscribe(
        [&](TrackListProjectionDeltaBatch const& batch)
        {
          if (batch.deltas.empty())
          {
            return;
          }

          if (std::holds_alternative<ProjectionReset>(batch.deltas.front()))
          {
            receivedReset = true;
            receivedSize = proj.size();
          }
        });

      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Artist, .sortBy = {TrackSortTerm{.field = TrackSortField::Artist}}});

      CHECK(receivedReset);
      CHECK(receivedSize == projSize);
    }
  }
} // namespace ao::rt::test
