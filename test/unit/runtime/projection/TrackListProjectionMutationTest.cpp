// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/projection/TrackListProjectionTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("TrackListProjection - batch insertion keeps sorted rows and index map", "[runtime][unit][projection]")
  {
    auto env = TrackListProjectionFixture{};
    auto id1 = env.libraryFixture.addTrack(library::test::makeTrackSpec("B", 2020));
    auto id2 = env.libraryFixture.addTrack(library::test::makeTrackSpec("D", 2020));
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title, .ascending = true}}});

    REQUIRE(proj.size() == 2);
    CHECK(proj.trackIdAt(0) == id1);
    CHECK(proj.trackIdAt(1) == id2);

    auto id3 = env.libraryFixture.addTrack(library::test::makeTrackSpec("A", 2020));
    auto id4 = env.libraryFixture.addTrack(library::test::makeTrackSpec("C", 2020));
    auto id5 = env.libraryFixture.addTrack(library::test::makeTrackSpec("E", 2020));

    // Simulate batch insertion from source
    // In our TrackListProjectionFixture, the filtered (SmartListSource) is the one attached to proj.
    // We notify the evaluator about the new tracks in the base source.
    env.source.batchInsert({{id3, id4, id5}});
    // SmartListSource reacts to source changes.

    // After updates, the projection should have updated incrementally
    REQUIRE(proj.size() == 5);
    CHECK(proj.trackIdAt(0) == id3); // A
    CHECK(proj.trackIdAt(1) == id1); // B
    CHECK(proj.trackIdAt(2) == id4); // C
    CHECK(proj.trackIdAt(3) == id2); // D
    CHECK(proj.trackIdAt(4) == id5); // E

    // Verify positionIndex
    CHECK(proj.indexOf(id3) == 0);
    CHECK(proj.indexOf(id1) == 1);
    CHECK(proj.indexOf(id4) == 2);
    CHECK(proj.indexOf(id2) == 3);
    CHECK(proj.indexOf(id5) == 4);
  }

  TEST_CASE("TrackListProjection - batch removal drops rows and stale index entries", "[runtime][unit][projection]")
  {
    auto env = TrackListProjectionFixture{};
    auto id1 = env.libraryFixture.addTrack(library::test::makeTrackSpec("A", 2020));
    auto id2 = env.libraryFixture.addTrack(library::test::makeTrackSpec("B", 2020));
    auto id3 = env.libraryFixture.addTrack(library::test::makeTrackSpec("C", 2020));
    auto id4 = env.libraryFixture.addTrack(library::test::makeTrackSpec("D", 2020));
    env.setupFiltered({{id1, id2, id3, id4}});

    auto proj = env.createProjection(ViewId{1});
    auto sub = proj.subscribe([](TrackListProjectionDeltaBatch const&) {});

    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title, .ascending = true}}});

    REQUIRE(proj.size() == 4);

    // Remove B and D
    // In our improved MutableTrackSource, we can now use batchRemove
    // But since filtered is a SmartListSource, it will handle the removal notification.
    env.source.batchRemove({{id2, id4}});

    REQUIRE(proj.size() == 2);
    CHECK(proj.trackIdAt(0) == id1);
    CHECK(proj.trackIdAt(1) == id3);

    CHECK(proj.indexOf(id1) == 0);
    CHECK(proj.indexOf(id3) == 1);
    CHECK_FALSE(proj.indexOf(id2).has_value());
    CHECK_FALSE(proj.indexOf(id4).has_value());
  }

  TEST_CASE("TrackListProjection - single and batch mutations without grouping", "[runtime][unit][projection]")
  {
    auto env = TrackListProjectionFixture{};
    auto id1 = env.libraryFixture.addTrack(library::test::makeTrackSpec("A", 2020));
    auto id2 = env.libraryFixture.addTrack(library::test::makeTrackSpec("C", 2020));
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto sub = proj.subscribe([&](TrackListProjectionDeltaBatch const& batch) { batches.push_back(batch); });

    // No grouping, yes comparator
    proj.setPresentation(TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title, .ascending = true}}});
    batches.clear();

    SECTION("single insertion via single method")
    {
      auto id3 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "B", .year = 2020});
      env.source.singleInsert(id3);
      REQUIRE(proj.size() == 3);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id3);
      CHECK(proj.trackIdAt(2) == id2);
      CHECK(proj.indexOf(id3) == 1);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionInsertRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 1);
      CHECK(delta->range.count == 1);
    }

    SECTION("single insertion via batch method")
    {
      auto id3 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "B", .year = 2020});
      auto arr = std::array{id3};
      env.source.batchInsert(arr);
      REQUIRE(proj.size() == 3);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id3);
      CHECK(proj.trackIdAt(2) == id2);
      CHECK(proj.indexOf(id3) == 1);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionInsertRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 1);
      CHECK(delta->range.count == 1);
    }

    SECTION("single removal via single method")
    {
      env.source.singleRemove(id1);
      REQUIRE(proj.size() == 1);
      CHECK(proj.trackIdAt(0) == id2);
      CHECK_FALSE(proj.indexOf(id1).has_value());
      CHECK(proj.indexOf(id2) == 0);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionRemoveRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 1);
    }

    SECTION("single removal via batch method")
    {
      auto arr = std::array{id1};
      env.source.batchRemove(arr);
      REQUIRE(proj.size() == 1);
      CHECK(proj.trackIdAt(0) == id2);
      CHECK_FALSE(proj.indexOf(id1).has_value());
      CHECK(proj.indexOf(id2) == 0);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionRemoveRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 1);
    }

    SECTION("single update of non-sort field preserves order")
    {
      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.artist = "Updated Artist"; });
      env.source.singleUpdate(id1);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionUpdateRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 1);
    }

    SECTION("single update breaking order")
    {
      // A -> Z
      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.title = "Z"; });
      env.source.singleUpdate(id1);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id2); // C
      CHECK(proj.trackIdAt(1) == id1); // Z
      CHECK(proj.indexOf(id1) == 1);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.front().deltas.size() == 2);
      CHECK(std::holds_alternative<ProjectionRemoveRange>(batches.front().deltas[0]));
      CHECK(std::holds_alternative<ProjectionInsertRange>(batches.front().deltas[1]));
    }

    SECTION("single update via batch method")
    {
      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.artist = "Updated Artist"; });
      auto arr = std::array{id1};
      env.source.batchUpdate(arr);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionUpdateRange>(batches.back().deltas.front()));
    }

    SECTION("batch update of non-sort fields preserves order and coalesces update ranges")
    {
      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.artist = "Updated Artist A"; });
      env.libraryFixture.updateTrack(id2, [](library::test::TrackSpec& s) { s.artist = "Updated Artist C"; });

      auto arr = std::array{id1, id2};
      env.source.batchUpdate(arr);

      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.back().deltas.size() == 1);
      auto const* delta = std::get_if<ProjectionUpdateRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 2);
    }

    SECTION("batch update of sort fields moves entries without reset")
    {
      // A, C -> Z, B
      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.title = "Z"; });
      env.libraryFixture.updateTrack(id2, [](library::test::TrackSpec& s) { s.title = "B"; });

      auto arr = std::array{id1, id2};
      env.source.batchUpdate(arr);

      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id2);
      CHECK(proj.trackIdAt(1) == id1);
      CHECK(proj.indexOf(id1) == 1);
      CHECK(proj.indexOf(id2) == 0);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.back().deltas.size() == 2);
      CHECK(std::holds_alternative<ProjectionRemoveRange>(batches.back().deltas[0]));
      CHECK(std::holds_alternative<ProjectionInsertRange>(batches.back().deltas[1]));
    }

    SECTION("batch update of stable rows coalesces final update coordinates")
    {
      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.artist = "Updated Artist A"; });
      env.libraryFixture.updateTrack(id2, [](library::test::TrackSpec& s) { s.title = "B"; });

      auto arr = std::array{id1, id2};
      env.source.batchUpdate(arr);

      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.back().deltas.size() == 1);
      auto const* update = std::get_if<ProjectionUpdateRange>(&batches.back().deltas.front());
      REQUIRE(update != nullptr);
      CHECK(update->range.start == 0);
      CHECK(update->range.count == 2);
    }

    SECTION("batch insertion with grouping")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});
      batches.clear();

      auto id3 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "B", .genre = "Pop", .year = 2020});
      auto id4 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "D", .genre = "Rock", .year = 2020});
      auto arr = std::array{id3, id4};
      env.source.batchInsert(arr);
      REQUIRE(proj.size() == 4);
      REQUIRE(proj.groupCount() == 3);
      CHECK(proj.groupAt(0).primaryText == "Unknown Genre");
      CHECK(proj.groupAt(1).primaryText == "Pop");
      CHECK(proj.groupAt(2).primaryText == "Rock");
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("batch insertion into existing group coalesces insert range")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});
      batches.clear();

      auto id3 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "B", .year = 2020});
      auto id4 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "D", .year = 2020});
      auto arr = std::array{id3, id4};
      env.source.batchInsert(arr);
      REQUIRE(proj.size() == 4);
      REQUIRE(proj.groupCount() == 1);
      CHECK(proj.groupAt(0).rows.start == 0);
      CHECK(proj.groupAt(0).rows.count == 4);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.back().deltas.size() == 1);
      auto const* delta = std::get_if<ProjectionInsertRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 2);
      CHECK(delta->range.count == 2);
    }

    SECTION("batch removal with grouping")
    {
      proj.setPresentation(
        TrackPresentationSpec{.groupBy = TrackGroupKey::Composer,
                              .sortBy = {TrackSortTerm{.field = TrackSortField::Composer, .ascending = true}}});
      batches.clear();

      auto arr = std::array{id1, id2};
      env.source.batchRemove(arr);
      REQUIRE(proj.size() == 0);
      CHECK(proj.groupCount() == 0);
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("batch removal from existing group coalesces remove range")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});

      auto id3 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "B", .year = 2020});
      auto id4 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "D", .year = 2020});
      auto insertArr = std::array{id3, id4};
      env.source.batchInsert(insertArr);
      REQUIRE(proj.groupCount() == 1);
      batches.clear();

      auto removeArr = std::array{id3, id4};
      env.source.batchRemove(removeArr);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      REQUIRE(proj.groupCount() == 1);
      CHECK(proj.groupAt(0).rows.start == 0);
      CHECK(proj.groupAt(0).rows.count == 2);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.back().deltas.size() == 1);
      auto const* delta = std::get_if<ProjectionRemoveRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 2);
      CHECK(delta->range.count == 2);
    }

    SECTION("batch update with grouping")
    {
      proj.setPresentation(
        TrackPresentationSpec{.groupBy = TrackGroupKey::Artist,
                              .sortBy = {TrackSortTerm{.field = TrackSortField::Artist, .ascending = true}}});
      batches.clear();

      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.artist = "Zulu"; });
      env.libraryFixture.updateTrack(id2, [](library::test::TrackSpec& s) { s.artist = "Bravo"; });
      auto arr = std::array{id1, id2};
      env.source.batchUpdate(arr);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id2);
      CHECK(proj.trackIdAt(1) == id1);
      REQUIRE(proj.groupCount() == 2);
      CHECK(proj.groupAt(0).primaryText == "Bravo");
      CHECK(proj.groupAt(1).primaryText == "Zulu");
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("single update with grouping")
    {
      proj.setPresentation(
        TrackPresentationSpec{.groupBy = TrackGroupKey::Artist,
                              .sortBy = {TrackSortTerm{.field = TrackSortField::Artist, .ascending = true}}});
      batches.clear();

      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.artist = "Zulu"; });
      env.source.singleUpdate(id1);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id2);
      CHECK(proj.trackIdAt(1) == id1);
      REQUIRE(proj.groupCount() == 2);
      CHECK(proj.groupAt(0).primaryText == "Artist");
      CHECK(proj.groupAt(1).primaryText == "Zulu");
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("single insertion into existing group publishes insert range")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});
      batches.clear();

      auto id3 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "B", .year = 2020});
      env.source.singleInsert(id3);
      REQUIRE(proj.size() == 3);
      REQUIRE(proj.groupCount() == 1);
      CHECK(proj.groupAt(0).rows.start == 0);
      CHECK(proj.groupAt(0).rows.count == 3);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.back().deltas.size() == 1);
      auto const* delta = std::get_if<ProjectionInsertRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 2);
      CHECK(delta->range.count == 1);
    }

    SECTION("single insertion with grouping")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});
      batches.clear();

      auto id3 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "B", .genre = "Pop", .year = 2020});
      env.source.singleInsert(id3);
      REQUIRE(proj.size() == 3);
      REQUIRE(proj.groupCount() == 2);
      CHECK(proj.groupAt(0).primaryText == "Unknown Genre");
      CHECK(proj.groupAt(1).primaryText == "Pop");
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("single removal from existing group publishes remove range")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});
      batches.clear();

      env.source.singleRemove(id1);
      REQUIRE(proj.size() == 1);
      CHECK(proj.trackIdAt(0) == id2);
      REQUIRE(proj.groupCount() == 1);
      CHECK(proj.groupAt(0).rows.start == 0);
      CHECK(proj.groupAt(0).rows.count == 1);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.back().deltas.size() == 1);
      auto const* delta = std::get_if<ProjectionRemoveRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 1);
    }

    SECTION("single removal with grouping")
    {
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::Genre, .sortBy = {TrackSortTerm{.field = TrackSortField::Genre, .ascending = true}}});

      auto id3 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "B", .genre = "Pop", .year = 2020});
      env.source.singleInsert(id3);
      REQUIRE(proj.groupCount() == 2);
      batches.clear();

      env.source.singleRemove(id3);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      REQUIRE(proj.groupCount() == 1);
      CHECK(proj.groupAt(0).primaryText == "Unknown Genre");
      REQUIRE(batches.size() == 1);
      CHECK(std::holds_alternative<ProjectionReset>(batches.back().deltas.front()));
    }

    SECTION("batch update inside existing group coalesces update range")
    {
      proj.setPresentation(
        TrackPresentationSpec{.groupBy = TrackGroupKey::Artist,
                              .sortBy = {TrackSortTerm{.field = TrackSortField::Artist, .ascending = true}}});
      batches.clear();

      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.title = "AA"; });
      env.libraryFixture.updateTrack(id2, [](library::test::TrackSpec& s) { s.title = "CC"; });
      auto arr = std::array{id1, id2};
      env.source.batchUpdate(arr);
      REQUIRE(proj.size() == 2);
      REQUIRE(proj.groupCount() == 1);
      CHECK(proj.groupAt(0).rows.count == 2);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.back().deltas.size() == 1);
      auto const* delta = std::get_if<ProjectionUpdateRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 2);
    }

    SECTION("batch update moving rows inside existing group avoids reset")
    {
      proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::Artist,
                                                 .sortBy = {
                                                   TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                                                   TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                                                 }});
      batches.clear();

      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.title = "Z"; });
      env.libraryFixture.updateTrack(id2, [](library::test::TrackSpec& s) { s.title = "B"; });
      auto arr = std::array{id1, id2};
      env.source.batchUpdate(arr);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id2);
      CHECK(proj.trackIdAt(1) == id1);
      REQUIRE(proj.groupCount() == 1);
      CHECK(proj.groupAt(0).rows.count == 2);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.back().deltas.size() == 2);
      CHECK(std::holds_alternative<ProjectionRemoveRange>(batches.back().deltas[0]));
      CHECK(std::holds_alternative<ProjectionInsertRange>(batches.back().deltas[1]));
    }

    SECTION("batch insert without comparator")
    {
      proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::None});
      batches.clear();

      auto id3 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "B", .year = 2020});
      auto id4 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "D", .year = 2020});
      auto arr = std::array{id3, id4};
      env.source.batchInsert(arr);
      REQUIRE(proj.size() == 4);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      CHECK(proj.trackIdAt(2) == id3);
      CHECK(proj.trackIdAt(3) == id4);
      REQUIRE(batches.size() == 1);
      REQUIRE(batches.back().deltas.size() == 1);
      auto const* insertion = std::get_if<ProjectionInsertRange>(&batches.back().deltas.front());
      REQUIRE(insertion != nullptr);
      CHECK(insertion->range.start == 2);
      CHECK(insertion->range.count == 2);
    }

    SECTION("single update without comparator")
    {
      proj.setPresentation(TrackPresentationSpec{.groupBy = TrackGroupKey::None});
      batches.clear();

      env.libraryFixture.updateTrack(id1, [](library::test::TrackSpec& s) { s.title = "AA"; });
      env.source.singleUpdate(id1);
      REQUIRE(proj.size() == 2);
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
      REQUIRE(batches.size() == 1);
      auto const* delta = std::get_if<ProjectionUpdateRange>(&batches.back().deltas.front());
      REQUIRE(delta != nullptr);
      CHECK(delta->range.start == 0);
      CHECK(delta->range.count == 1);
    }

    SECTION("Destructor coverage")
    {
      auto proj2Ptr = std::make_unique<LiveTrackListProjection>(
        ViewId{2}, TrackSourceLease{env.filteredPtr}, env.libraryFixture.library());
      proj2Ptr.reset();
    }
  }
} // namespace ao::rt::test
