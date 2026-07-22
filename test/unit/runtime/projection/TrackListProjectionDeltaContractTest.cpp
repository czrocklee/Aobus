// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    class QueryCountingTrackSource final : public TrackSource
    {
    public:
      explicit QueryCountingTrackSource(TrackId trackId)
        : _trackId{trackId}
      {
      }

      std::size_t size() const override
      {
        ++_queryCount;
        return 1;
      }

      TrackId trackIdAt(std::size_t /*index*/) const override
      {
        ++_queryCount;
        return _trackId;
      }

      std::optional<std::size_t> indexOf(TrackId trackId) const override
      {
        ++_queryCount;
        return trackId == _trackId ? std::optional<std::size_t>{0} : std::nullopt;
      }

      std::size_t queryCount() const noexcept { return _queryCount; }

    private:
      TrackId _trackId{};
      mutable std::size_t _queryCount = 0;
    };

    std::vector<TrackId> projectionTrackIds(TrackListProjection const& projection)
    {
      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(projection.size());

      for (std::size_t index = 0; index < projection.size(); ++index)
      {
        trackIds.push_back(projection.trackIdAt(index));
      }

      return trackIds;
    }
  } // namespace

  TEST_CASE("TrackListProjection - delta validation enforces sequential ranges and singleton lifecycle batches",
            "[runtime][unit][projection][delta]")
  {
    auto const move = TrackListProjectionDeltaBatch{
      .deltas =
        {
          ProjectionRemoveRange{TrackRowRange{.start = 1, .count = 1}},
          ProjectionInsertRange{TrackRowRange{.start = 2, .count = 1}},
        },
    };
    auto const invalidCoordinate = TrackListProjectionDeltaBatch{
      .deltas = {ProjectionInsertRange{TrackRowRange{.start = 4, .count = 1}}},
    };
    auto const invalidReset = TrackListProjectionDeltaBatch{
      .deltas =
        {
          ProjectionReset{},
          ProjectionUpdateRange{TrackRowRange{.start = 0, .count = 1}},
        },
    };
    auto const invalidTerminal = TrackListProjectionDeltaBatch{
      .deltas =
        {
          ProjectionSourceInvalidated{},
          ProjectionRemoveRange{TrackRowRange{.start = 0, .count = 1}},
        },
    };

    CHECK(validateTrackListProjectionDeltaBatch(move, 3));
    CHECK(validateTrackListProjectionDeltaBatch(TrackListProjectionDeltaBatch{.deltas = {ProjectionReset{}}}, 3));
    CHECK(validateTrackListProjectionDeltaBatch(
      TrackListProjectionDeltaBatch{.deltas = {ProjectionSourceInvalidated{}}}, 3));
    CHECK_FALSE(validateTrackListProjectionDeltaBatch(invalidCoordinate, 3));
    CHECK_FALSE(validateTrackListProjectionDeltaBatch(invalidReset, 3));
    CHECK_FALSE(validateTrackListProjectionDeltaBatch(invalidTerminal, 3));
  }

  TEST_CASE("TrackListProjection - empty sort mirrors a middle source insertion exactly",
            "[runtime][unit][projection][source-order]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("First", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("Second", 2020));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("Third", 2020));
    auto sourcePtr = makeMutableTrackSource({third, first});
    auto projection = LiveTrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      projection.subscribe([&batches](TrackListProjectionDeltaBatch const& batch) { batches.push_back(batch); });
    batches.clear();

    sourcePtr->insert(second, 1);

    auto const expected = std::vector{third, second, first};
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    auto const& insertion = std::get<ProjectionInsertRange>(batches.front().deltas.front());
    CHECK(insertion.range.start == 1);
    CHECK(insertion.range.count == 1);
    CHECK(projectionTrackIds(projection) == expected);
  }

  TEST_CASE("TrackListProjection - empty sort preserves source positions through update removal and reset",
            "[runtime][unit][projection][source-order]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("First", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("Second", 2020));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("Third", 2020));
    auto const fourth = libraryFixture.addTrack(library::test::makeTrackSpec("Fourth", 2020));
    auto sourcePtr = makeMutableTrackSource({third, first, fourth, second});
    auto projection = LiveTrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    CHECK(projectionTrackIds(projection) == std::vector{third, first, fourth, second});
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      projection.subscribe([&batches](TrackListProjectionDeltaBatch const& batch) { batches.push_back(batch); });
    batches.clear();

    libraryFixture.updateTrack(third, [](library::test::TrackSpec& spec) { spec.title = "Updated Third"; });
    sourcePtr->replaceWithBatch(std::array{third, fourth, second},
                                TrackSourceDeltaBatch{
                                  .deltas =
                                    {
                                      SourceUpdateRange{.start = 0, .trackIds = {third}},
                                      SourceRemoveRange{.start = 1, .trackIds = {first}},
                                    },
                                });

    REQUIRE(batches.size() == 1);
    CHECK(validateTrackListProjectionDeltaBatch(batches.front(), 4));
    CHECK(projectionTrackIds(projection) == std::vector{third, fourth, second});

    batches.clear();
    sourcePtr->reset(std::array{second, third, fourth});

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<ProjectionReset>(batches.front().deltas.front()));
    CHECK(projectionTrackIds(projection) == std::vector{second, third, fourth});
  }

  TEST_CASE("TrackListProjection - subscription observes a source mutation triggered by its initial reset",
            "[runtime][unit][projection][subscription]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("First", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("Second", 2020));
    auto sourcePtr = makeMutableTrackSource({first});
    auto projection = LiveTrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    bool insertedDuringReset = false;

    [[maybe_unused]] auto subscription = projection.subscribe(
      [&](TrackListProjectionDeltaBatch const& batch)
      {
        batches.push_back(batch);

        if (!insertedDuringReset && std::holds_alternative<ProjectionReset>(batch.deltas.front()))
        {
          insertedDuringReset = true;
          sourcePtr->insert(second, 1);
        }
      });

    auto const expected = std::vector{first, second};
    REQUIRE(batches.size() == 2);
    REQUIRE(batches[0].deltas.size() == 1);
    REQUIRE(batches[1].deltas.size() == 1);
    CHECK(std::holds_alternative<ProjectionReset>(batches[0].deltas.front()));
    auto const& insertion = std::get<ProjectionInsertRange>(batches[1].deltas.front());
    CHECK(insertion.range.start == 1);
    CHECK(insertion.range.count == 1);
    CHECK(projectionTrackIds(projection) == expected);
  }

  TEST_CASE("TrackListProjection - empty sort publishes a source move as one sequential batch",
            "[runtime][unit][projection][source-order]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("First", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("Second", 2020));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("Third", 2020));
    auto const fourth = libraryFixture.addTrack(library::test::makeTrackSpec("Fourth", 2020));
    auto sourcePtr = makeMutableTrackSource({first, second, third, fourth});
    auto projection = LiveTrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      projection.subscribe([&batches](TrackListProjectionDeltaBatch const& batch) { batches.push_back(batch); });
    batches.clear();

    sourcePtr->replaceWithBatch(std::vector{second, third, first, fourth},
                                TrackSourceDeltaBatch{
                                  .deltas =
                                    {
                                      SourceRemoveRange{.start = 0, .trackIds = {first}},
                                      SourceInsertRange{.start = 2, .trackIds = {first}},
                                    },
                                });

    auto const expected = std::vector{second, third, first, fourth};
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 2);
    auto const& removal = std::get<ProjectionRemoveRange>(batches.front().deltas[0]);
    auto const& insertion = std::get<ProjectionInsertRange>(batches.front().deltas[1]);
    CHECK(removal.range.start == 0);
    CHECK(removal.range.count == 1);
    CHECK(insertion.range.start == 2);
    CHECK(insertion.range.count == 1);
    CHECK(validateTrackListProjectionDeltaBatch(batches.front(), 4));
    CHECK(projectionTrackIds(projection) == expected);
  }

  TEST_CASE("TrackListProjection - sorted source-only reorder does not publish", "[runtime][unit][projection][sorted]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("A", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("B", 2020));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("C", 2020));
    auto sourcePtr = makeMutableTrackSource({third, first, second});
    auto projection = LiveTrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    projection.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None,
      .sortBy = {TrackSortTerm{.field = TrackSortField::Title}},
    });
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      projection.subscribe([&batches](TrackListProjectionDeltaBatch const& batch) { batches.push_back(batch); });
    batches.clear();

    sourcePtr->replaceWithBatch(std::vector{first, second, third},
                                TrackSourceDeltaBatch{
                                  .deltas =
                                    {
                                      SourceRemoveRange{.start = 0, .trackIds = {third}},
                                      SourceInsertRange{.start = 2, .trackIds = {third}},
                                    },
                                });

    auto const expected = std::vector{first, second, third};
    CHECK(projectionTrackIds(projection) == expected);
    CHECK(batches.empty());
  }

  TEST_CASE("TrackListProjection - sorted metadata move publishes one atomic projection batch",
            "[runtime][unit][projection][sorted]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("A", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("C", 2020));
    auto sourcePtr = makeMutableTrackSource({first, second});
    auto projection = LiveTrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    projection.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None,
      .sortBy = {TrackSortTerm{.field = TrackSortField::Title}},
    });
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      projection.subscribe([&batches](TrackListProjectionDeltaBatch const& batch) { batches.push_back(batch); });
    batches.clear();

    libraryFixture.updateTrack(first, [](library::test::TrackSpec& spec) { spec.title = "Z"; });
    sourcePtr->update(first);

    auto const expected = std::vector{second, first};
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 2);
    auto const& removal = std::get<ProjectionRemoveRange>(batches.front().deltas[0]);
    auto const& insertion = std::get<ProjectionInsertRange>(batches.front().deltas[1]);
    CHECK(removal.range.start == 0);
    CHECK(removal.range.count == 1);
    CHECK(insertion.range.start == 1);
    CHECK(insertion.range.count == 1);
    CHECK(validateTrackListProjectionDeltaBatch(batches.front(), 2));
    CHECK(projectionTrackIds(projection) == expected);
  }

  TEST_CASE("TrackListProjection - detached construction applies only captured order",
            "[runtime][unit][projection][detached]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("A", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("B", 2020));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("C", 2020));
    auto sourcePtr = makeMutableTrackSource({third, first, second});

    auto projection = LiveTrackListProjection{
      kInvalidViewId,
      TrackSourceLease{sourcePtr},
      libraryFixture.library(),
      TrackOrderSpec{.sortBy = {TrackSortTerm{.field = TrackSortField::Title}}},
    };

    auto const expected = std::vector{first, second, third};
    CHECK(projection.viewId() == kInvalidViewId);
    CHECK(projection.presentation().groupBy == TrackGroupKey::None);
    CHECK(projection.presentation().sortBy == std::vector{TrackSortTerm{.field = TrackSortField::Title}});
    CHECK(projection.groupCount() == 0);
    CHECK(projectionTrackIds(projection) == expected);
  }

  TEST_CASE("TrackListProjection - construction never queries an already invalidated source",
            "[runtime][unit][projection][lifecycle]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto sourcePtr = std::make_shared<QueryCountingTrackSource>(TrackId{99});
    sourcePtr->invalidate();

    auto projection = LiveTrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      projection.subscribe([&batches](TrackListProjectionDeltaBatch const& batch) { batches.push_back(batch); });
    projection.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None,
      .sortBy = {TrackSortTerm{.field = TrackSortField::Title}},
    });

    CHECK(sourcePtr->queryCount() == 0);
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<ProjectionSourceInvalidated>(batches.front().deltas.front()));
  }
} // namespace ao::rt::test
