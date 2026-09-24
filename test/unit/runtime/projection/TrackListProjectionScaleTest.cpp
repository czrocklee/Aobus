// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    template<typename State>
    concept OwnsMaterializedTrackIds = requires(State const& state) { state.trackIds; };

    static_assert(!OwnsMaterializedTrackIds<PlaybackSuccessionState>);

    constexpr std::int32_t kScaleTrackCount = 10000;

    template<typename MakeSpec>
    std::vector<TrackId> addScaleTracks(library::MusicLibrary& library, std::int32_t count, MakeSpec const& makeSpec)
    {
      auto transaction = library::test::writeTransaction(library);
      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(static_cast<std::size_t>(count));

      REQUIRE(transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();

          for (std::int32_t const index : std::views::iota(std::int32_t{0}, count))
          {
            auto builder = library::TrackBuilder::makeEmpty();
            auto const spec = makeSpec(index);
            library::test::applyTrackSpec(builder, spec);

            auto createRes = writer.create(builder, library::FileManifestBuilder::makeEmpty());
            REQUIRE(createRes);
            trackIds.push_back(*createRes);
          }

          return {};
        }));

      REQUIRE(transaction.commit());
      return trackIds;
    }

    struct ScaleProjectionFixture final
    {
      ScaleProjectionFixture()
      {
        trackIds = addScaleTracks(libraryFixture.library(),
                                  kScaleTrackCount,
                                  [](std::int32_t index)
                                  {
                                    return library::test::TrackSpec{
                                      .title = std::format("Track {:05d}", index),
                                      .artist = std::format("Artist {:03d}", index % 100),
                                      .album = std::format("Album {:03d}", index % 500),
                                      .uri = std::format("track-{:05d}.flac", index),
                                    };
                                  });
        sourcePtr = std::make_shared<MutableTrackSource>();
        sourcePtr->setInitial(trackIds);
        projectionPtr = std::make_unique<TrackListProjection>(
          kInvalidViewId,
          TrackSourceLease{sourcePtr},
          libraryFixture.library(),
          TrackOrderSpec{.sortBy = {TrackSortTerm{.field = TrackSortField::Title}}});
      }

      std::vector<TrackId> addTracks(std::int32_t count, auto const& makeSpec)
      {
        return addScaleTracks(libraryFixture.library(), count, makeSpec);
      }

      TrackListProjection& projection() const noexcept { return *projectionPtr; }

      MusicLibraryFixture libraryFixture;
      std::vector<TrackId> trackIds;
      std::shared_ptr<MutableTrackSource> sourcePtr;
      std::unique_ptr<TrackListProjection> projectionPtr;
    };
  } // namespace

  TEST_CASE("TrackListProjection - 10k sorted insertion publishes one exact leading range",
            "[runtime][unit][projection][scale]")
  {
    auto fixture = ScaleProjectionFixture{};
    auto& projection = fixture.projection();
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto const subscription = projection.subscribe([&batches](TrackListProjectionDeltaBatch const& batch) noexcept
                                                   { batches.push_back(batch); });
    batches.clear();

    auto const insertedIds = fixture.addTracks(100,
                                               [](std::int32_t index)
                                               {
                                                 return library::test::TrackSpec{
                                                   .title = std::format("New Track {:05d}", index),
                                                   .uri = std::format("new-track-{:05d}.flac", index),
                                                 };
                                               });

    fixture.sourcePtr->batchInsert(insertedIds);

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    REQUIRE(std::holds_alternative<ProjectionInsertRange>(batches.front().deltas.front()));
    auto const& insertion = std::get<ProjectionInsertRange>(batches.front().deltas.front());
    CHECK(insertion.range.start == 0);
    CHECK(insertion.range.count == insertedIds.size());
    REQUIRE(projection.size() == static_cast<std::size_t>(kScaleTrackCount) + insertedIds.size());

    for (std::size_t index = 0; index < insertedIds.size(); ++index)
    {
      CHECK(projection.trackIdAt(index) == insertedIds[index]);
      CHECK(projection.indexOf(insertedIds[index]) == index);
    }

    CHECK(projection.trackIdAt(insertedIds.size()) == fixture.trackIds.front());
    CHECK(projection.indexOf(fixture.trackIds.front()) == insertedIds.size());
    CHECK(projection.trackIdAt(projection.size() - 1) == fixture.trackIds.back());
    CHECK(projection.indexOf(fixture.trackIds.back()) == projection.size() - 1);
  }

  TEST_CASE("TrackListProjection - 10k index lookup retains exact boundary and middle positions",
            "[runtime][unit][projection][scale]")
  {
    auto fixture = ScaleProjectionFixture{};
    auto& projection = fixture.projection();
    REQUIRE(projection.size() == static_cast<std::size_t>(kScaleTrackCount));
    CHECK(projection.viewId() == kInvalidViewId);
    auto const firstId = fixture.trackIds.front();
    auto const middleIndex = static_cast<std::size_t>(kScaleTrackCount / 2);
    auto const middleId = fixture.trackIds[middleIndex];
    auto const lastIndex = static_cast<std::size_t>(kScaleTrackCount - 1);
    auto const lastId = fixture.trackIds.back();
    CHECK(projection.trackIdAt(0) == firstId);
    CHECK(projection.trackIdAt(middleIndex) == middleId);
    CHECK(projection.trackIdAt(lastIndex) == lastId);

    for (std::int32_t iteration = 0; iteration < 10; ++iteration)
    {
      CHECK(projection.indexOf(firstId) == std::size_t{0});
      CHECK(projection.indexOf(middleId) == middleIndex);
      CHECK(projection.indexOf(lastId) == lastIndex);
    }

    CHECK_FALSE(projection.indexOf(TrackId{999999}).has_value());
  }

  TEST_CASE("TrackListProjection - 10k detached mutations publish exact remove and update ranges",
            "[runtime][unit][projection][scale]")
  {
    auto fixture = ScaleProjectionFixture{};
    auto& projection = fixture.projection();
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto const subscription = projection.subscribe([&batches](TrackListProjectionDeltaBatch const& batch) noexcept
                                                   { batches.push_back(batch); });
    batches.clear();

    auto const removedIds = std::array{fixture.trackIds[100], fixture.trackIds[5000], fixture.trackIds[9999]};
    fixture.sourcePtr->batchRemove(removedIds);

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 3);
    REQUIRE(std::holds_alternative<ProjectionRemoveRange>(batches.front().deltas[0]));
    REQUIRE(std::holds_alternative<ProjectionRemoveRange>(batches.front().deltas[1]));
    REQUIRE(std::holds_alternative<ProjectionRemoveRange>(batches.front().deltas[2]));
    auto const& lastRemoval = std::get<ProjectionRemoveRange>(batches.front().deltas[0]);
    auto const& middleRemoval = std::get<ProjectionRemoveRange>(batches.front().deltas[1]);
    auto const& firstRemoval = std::get<ProjectionRemoveRange>(batches.front().deltas[2]);
    CHECK(lastRemoval.range.start == 9999);
    CHECK(lastRemoval.range.count == 1);
    CHECK(middleRemoval.range.start == 5000);
    CHECK(middleRemoval.range.count == 1);
    CHECK(firstRemoval.range.start == 100);
    CHECK(firstRemoval.range.count == 1);
    CHECK(projection.size() == static_cast<std::size_t>(kScaleTrackCount) - removedIds.size());

    for (auto const trackId : removedIds)
    {
      CHECK_FALSE(projection.indexOf(trackId));
    }

    batches.clear();
    auto const updatedIds = std::array{fixture.trackIds[10], fixture.trackIds[9000]};
    fixture.sourcePtr->batchUpdate(updatedIds);

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 2);
    REQUIRE(std::holds_alternative<ProjectionUpdateRange>(batches.front().deltas[0]));
    REQUIRE(std::holds_alternative<ProjectionUpdateRange>(batches.front().deltas[1]));
    auto const& firstUpdate = std::get<ProjectionUpdateRange>(batches.front().deltas[0]);
    auto const& secondUpdate = std::get<ProjectionUpdateRange>(batches.front().deltas[1]);
    CHECK(firstUpdate.range.start == 10);
    CHECK(firstUpdate.range.count == 1);
    CHECK(secondUpdate.range.start == 8998);
    CHECK(secondUpdate.range.count == 1);
    CHECK(projection.trackIdAt(firstUpdate.range.start) == updatedIds[0]);
    CHECK(projection.trackIdAt(secondUpdate.range.start) == updatedIds[1]);
  }

  TEST_CASE("TrackListProjection - 10k presentation change publishes one reset with rebuilt size visible",
            "[runtime][unit][projection][scale]")
  {
    auto fixture = ScaleProjectionFixture{};
    auto& projection = fixture.projection();
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto callbackSizes = std::vector<std::size_t>{};
    auto const subscription = projection.subscribe(
      [&](TrackListProjectionDeltaBatch const& batch) noexcept
      {
        batches.push_back(batch);
        callbackSizes.push_back(projection.size());
      });
    batches.clear();
    callbackSizes.clear();

    projection.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::Artist, .sortBy = {TrackSortTerm{.field = TrackSortField::Artist}}});

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<ProjectionReset>(batches.front().deltas.front()));
    REQUIRE(callbackSizes.size() == 1);
    CHECK(callbackSizes.front() == static_cast<std::size_t>(kScaleTrackCount));
    CHECK(projection.size() == static_cast<std::size_t>(kScaleTrackCount));
  }
} // namespace ao::rt::test
