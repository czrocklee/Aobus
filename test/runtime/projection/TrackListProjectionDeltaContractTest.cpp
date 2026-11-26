// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/library/TrackTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include "test/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
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

  TEST_CASE("TrackListProjection - delta validation enforces sequential ranges and singleton lifecycle batches")
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

    CHECK(isValidTrackListProjectionDeltaBatch(move, 3));
    CHECK(isValidTrackListProjectionDeltaBatch(TrackListProjectionDeltaBatch{.deltas = {ProjectionReset{}}}, 3));
    CHECK(isValidTrackListProjectionDeltaBatch(
      TrackListProjectionDeltaBatch{.deltas = {ProjectionSourceInvalidated{}}}, 3));
    CHECK_FALSE(isValidTrackListProjectionDeltaBatch(invalidCoordinate, 3));
    CHECK_FALSE(isValidTrackListProjectionDeltaBatch(invalidReset, 3));
    CHECK_FALSE(isValidTrackListProjectionDeltaBatch(invalidTerminal, 3));
  }

  TEST_CASE("TrackListProjection - empty sort mirrors a middle source insertion exactly")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("First", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("Second", 2020));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("Third", 2020));
    auto sourcePtr = makeMutableTrackSource({third, first});
    auto projection = TrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription = projection.subscribe(
      [&batches](TrackListProjectionDeltaBatch const& batch) noexcept { batches.push_back(batch); });
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

  TEST_CASE("TrackListProjection - empty sort preserves source positions through update removal and reset")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("First", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("Second", 2020));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("Third", 2020));
    auto const fourth = libraryFixture.addTrack(library::test::makeTrackSpec("Fourth", 2020));
    auto sourcePtr = makeMutableTrackSource({third, first, fourth, second});
    auto projection = TrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    CHECK(projectionTrackIds(projection) == std::vector{third, first, fourth, second});
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription = projection.subscribe(
      [&batches](TrackListProjectionDeltaBatch const& batch) noexcept { batches.push_back(batch); });
    batches.clear();

    libraryFixture.updateTrack(third, [](library::test::TrackSpec& spec) { spec.title = "Updated Third"; });
    sourcePtr->replaceWithBatch(std::array{third, fourth, second},
                                delta::RegularTrackEditScript{
                                  .edits = {delta::RemoveRange{.start = 1, .trackIds = {first}},
                                            delta::UpdateRange{.start = 0, .trackIds = {third}}},
                                });

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 2);
    REQUIRE(std::holds_alternative<ProjectionRemoveRange>(batches.front().deltas[0]));
    REQUIRE(std::holds_alternative<ProjectionUpdateRange>(batches.front().deltas[1]));
    auto const& removal = std::get<ProjectionRemoveRange>(batches.front().deltas[0]);
    auto const& update = std::get<ProjectionUpdateRange>(batches.front().deltas[1]);
    CHECK(removal.range.start == 1);
    CHECK(removal.range.count == 1);
    CHECK(update.range.start == 0);
    CHECK(update.range.count == 1);
    CHECK(isValidTrackListProjectionDeltaBatch(batches.front(), 4));
    CHECK(projectionTrackIds(projection) == std::vector{third, fourth, second});

    batches.clear();
    sourcePtr->reset(std::array{second, third, fourth});

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    auto const* reset = std::get_if<ProjectionReset>(&batches.front().deltas.front());
    REQUIRE(reset != nullptr);
    CHECK_FALSE(reset->optAffectedTrackIds);
    CHECK(projectionTrackIds(projection) == std::vector{second, third, fourth});
  }

  TEST_CASE("TrackListProjection - equal effective ordering still resets for changes beyond a column marker")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::TrackSpec{.title = "First", .album = "Album"});
    auto const second = libraryFixture.addTrack(library::test::TrackSpec{.title = "Second", .album = "Album"});
    auto sourcePtr = makeMutableTrackSource({second, first});
    auto projection = TrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    auto const initial = TrackPresentationSpec{
      .presetId = CustomPresetId{1},
      .groupBy = TrackGroupKey::Album,
      .sortBy = {{.field = TrackSortField::Title}},
      .visibleFields = {TrackField::Title, TrackField::Artist},
      .redundantFields = {TrackField::Album},
    };
    projection.setPresentation(initial);
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto const subscription =
      projection.subscribe([&](TrackListProjectionDeltaBatch const& batch) noexcept { batches.push_back(batch); });
    batches.clear();
    auto updated = initial;
    updated.optColumnSort = TrackColumnSort{TrackField::Title, true};

    SECTION("the same spec is reapplied")
    {
      updated = initial;
    }

    SECTION("the base sort changes under the marker")
    {
      updated.sortBy.front().ascending = false;
    }

    SECTION("visible fields change alongside the marker")
    {
      updated.visibleFields.push_back(TrackField::Duration);
    }

    SECTION("redundant fields change alongside the marker")
    {
      updated.redundantFields.push_back(TrackField::AlbumArtist);
    }

    SECTION("the preset changes alongside the marker")
    {
      updated.presetId = CustomPresetId{2};
    }

    REQUIRE(updated.effectiveSortBy() == initial.effectiveSortBy());
    projection.setPresentation(updated);

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    auto const* reset = std::get_if<ProjectionReset>(&batches.front().deltas.front());
    REQUIRE(reset != nullptr);
    CHECK_FALSE(reset->optAffectedTrackIds);
    CHECK(projection.presentation() == updated);
    CHECK(projectionTrackIds(projection) == std::vector{first, second});
  }

  TEST_CASE("TrackListProjection - an alias change still resets when explicit redundant fields match the fallback")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const id = libraryFixture.addTrack(library::test::TrackSpec{.title = "Track", .album = "Album"});
    auto sourcePtr = makeMutableTrackSource({id});
    auto projection = TrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;
    auto initial = albums;
    initial.optColumnSort = TrackColumnSort{TrackField::TrackNumber, true};
    auto updated = albums;
    updated.optColumnSort = TrackColumnSort{TrackField::DisplayTrackNumber, true};

    SECTION("specifying the inferred fields")
    {
      initial.redundantFields.clear();
    }

    SECTION("omitting the previously specified fields")
    {
      updated.redundantFields.clear();
    }

    REQUIRE(initial.effectiveSortBy() == updated.effectiveSortBy());
    projection.setPresentation(initial);
    auto expectedSnapshot = albums;
    expectedSnapshot.optColumnSort = initial.optColumnSort;
    REQUIRE(projection.presentation() == expectedSnapshot);
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto const subscription =
      projection.subscribe([&](TrackListProjectionDeltaBatch const& batch) noexcept { batches.push_back(batch); });
    batches.clear();

    projection.setPresentation(updated);

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<ProjectionReset>(batches.front().deltas.front()));
    expectedSnapshot.optColumnSort = updated.optColumnSort;
    CHECK(projection.presentation() == expectedSnapshot);
  }

  TEST_CASE("TrackListProjection - subscription observes a source mutation triggered by its initial reset")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("First", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("Second", 2020));
    auto sourcePtr = makeMutableTrackSource({first});
    auto projection = TrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    bool insertedDuringReset = false;

    [[maybe_unused]] auto subscription = projection.subscribe(
      [&](TrackListProjectionDeltaBatch const& batch) noexcept
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

  TEST_CASE("TrackListProjection - empty sort publishes a source move as one sequential batch")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("First", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("Second", 2020));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("Third", 2020));
    auto const fourth = libraryFixture.addTrack(library::test::makeTrackSpec("Fourth", 2020));
    auto sourcePtr = makeMutableTrackSource({first, second, third, fourth});
    auto projection = TrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription = projection.subscribe(
      [&batches](TrackListProjectionDeltaBatch const& batch) noexcept { batches.push_back(batch); });
    batches.clear();

    sourcePtr->replaceWithBatch(std::vector{second, third, first, fourth},
                                delta::RegularTrackEditScript{
                                  .edits = {delta::RemoveRange{.start = 0, .trackIds = {first}},
                                            delta::InsertRange{.start = 2, .trackIds = {first}}},
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
    CHECK(isValidTrackListProjectionDeltaBatch(batches.front(), 4));
    CHECK(projectionTrackIds(projection) == expected);
  }

  TEST_CASE("TrackListProjection - sorted source-only reorder does not publish")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("A", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("B", 2020));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("C", 2020));
    auto sourcePtr = makeMutableTrackSource({third, first, second});
    auto projection = TrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    projection.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None,
      .sortBy = {TrackSortTerm{.field = TrackSortField::Title}},
    });
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription = projection.subscribe(
      [&batches](TrackListProjectionDeltaBatch const& batch) noexcept { batches.push_back(batch); });
    batches.clear();

    sourcePtr->replaceWithBatch(std::vector{first, second, third},
                                delta::RegularTrackEditScript{
                                  .edits = {delta::RemoveRange{.start = 0, .trackIds = {third}},
                                            delta::InsertRange{.start = 2, .trackIds = {third}}},
                                });

    auto const expected = std::vector{first, second, third};
    CHECK(projectionTrackIds(projection) == expected);
    CHECK(batches.empty());
  }

  TEST_CASE("TrackListProjection - sorted metadata move publishes one atomic projection batch")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("A", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("C", 2020));
    auto sourcePtr = makeMutableTrackSource({first, second});
    auto projection = TrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    projection.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None,
      .sortBy = {TrackSortTerm{.field = TrackSortField::Title}},
    });
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    [[maybe_unused]] auto subscription = projection.subscribe(
      [&batches](TrackListProjectionDeltaBatch const& batch) noexcept { batches.push_back(batch); });
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
    CHECK(isValidTrackListProjectionDeltaBatch(batches.front(), 2));
    CHECK(projectionTrackIds(projection) == expected);
  }

  TEST_CASE("TrackListProjection - detached construction applies only captured order")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("A", 2020));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("B", 2020));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("C", 2020));
    auto sourcePtr = makeMutableTrackSource({third, first, second});

    auto projection = TrackListProjection{
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
} // namespace ao::rt::test
