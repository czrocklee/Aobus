// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/RuntimeOperationProbe.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/projection/TrackListProjectionTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void checkOrder(TrackListProjection const& projection, std::vector<TrackId> const& expected)
    {
      REQUIRE(projection.size() == expected.size());

      for (std::size_t index = 0; index < expected.size(); ++index)
      {
        CHECK(projection.trackIdAt(index) == expected[index]);
      }
    }

    using ProjectionCounts = ::ao::rt::detail::TrackListProjectionOperationCounts;

    // A moved row is remove-then-insert. An in-place key change is one update.
    struct ExpectedProjectionDelta final
    {
      bool moved = false;
      std::size_t from = 0;
      std::size_t to = 0;
    };

    void expectIncrementalDateOrder(TrackListProjectionFixture& fixture,
                                    TrackListProjection& projection,
                                    std::vector<TrackListProjectionDeltaBatch> const& batches,
                                    ProjectionCounts const& before,
                                    std::vector<TrackId> const& expected,
                                    ExpectedProjectionDelta const delta)
    {
      checkOrder(projection, expected);
      auto oracle = fixture.createProjection(ViewId{2});
      oracle.setPresentation(projection.presentation());
      checkOrder(oracle, expected);

      REQUIRE(batches.size() == 1);
      auto const& batch = batches.front();
      CHECK(isValidTrackListProjectionDeltaBatch(batch, expected.size()));
      REQUIRE_FALSE(batch.deltas.empty());
      CHECK_FALSE(std::holds_alternative<ProjectionReset>(batch.deltas.front()));
      CHECK_FALSE(std::holds_alternative<ProjectionSourceInvalidated>(batch.deltas.front()));

      if (delta.moved)
      {
        REQUIRE(batch.deltas.size() == 2);
        auto const& removal = std::get<ProjectionRemoveRange>(batch.deltas[0]);
        auto const& insertion = std::get<ProjectionInsertRange>(batch.deltas[1]);
        CHECK(removal.range.start == delta.from);
        CHECK(removal.range.count == 1);
        CHECK(insertion.range.start == delta.to);
        CHECK(insertion.range.count == 1);
      }
      else
      {
        REQUIRE(batch.deltas.size() == 1);
        auto const& update = std::get<ProjectionUpdateRange>(batch.deltas.front());
        CHECK(update.range.start == delta.from);
        CHECK(update.range.count == 1);
      }

      auto const after = ::ao::rt::detail::RuntimeOperationProbe::counts(projection);
      CHECK(after.fullProjectionRebuilds == before.fullProjectionRebuilds);
      CHECK(after.incrementalProjectionUpdates == before.incrementalProjectionUpdates + 1);
      CHECK(after.arenaRebases == before.arenaRebases);
    }

    TrackId addPerformanceTrack(TrackListProjectionFixture& fixture,
                                std::string album,
                                library::RecordingDate const date,
                                std::uint16_t const movement,
                                std::uint16_t const year = 2000)
    {
      return fixture.libraryFixture.addTrack(library::test::TrackSpec{
        .title = "Variation",
        .artist = "Glenn Gould",
        .album = std::move(album),
        .composer = "J. S. Bach",
        .work = "Goldberg Variations, BWV 988",
        .recordingDate = date,
        .credits = {{.name = "Conductor", .kind = library::CreditKind::Conductor},
                    {.name = "Glenn Gould", .kind = library::CreditKind::Soloist},
                    {.name = "Glenn Gould", .role = "piano"}},
        .year = year,
        .trackNumber = movement,
        .movementNumber = movement,
      });
    }
  } // namespace

  TEST_CASE("TrackListProjection - recording date orders partial tuples and keeps absence last in both directions",
            "[runtime][unit][projection]")
  {
    auto fixture = TrackListProjectionFixture{};
    auto const absent = addPerformanceTrack(fixture, "Compilation", {}, 1);
    auto const full = addPerformanceTrack(fixture, "Compilation", {1981, 5, 12}, 1);
    auto const year = addPerformanceTrack(fixture, "Compilation", {1981, 0, 0}, 1);
    auto const month = addPerformanceTrack(fixture, "Compilation", {1981, 5, 0}, 1);
    auto const older = addPerformanceTrack(fixture, "Compilation", {1955, 6, 0}, 1);
    auto const nextDay = addPerformanceTrack(fixture, "Compilation", {1981, 5, 13}, 1);
    fixture.setupFiltered({{absent, full, year, month, older, nextDay}});
    auto projection = fixture.createProjection(ViewId{1});
    auto const subscription = projection.subscribe([](TrackListProjectionDeltaBatch const&) noexcept {});

    projection.setPresentation(TrackPresentationSpec{
      .sortBy = {{TrackSortField::RecordingDate, true}},
    });
    checkOrder(projection, {older, year, month, full, nextDay, absent});
    projection.setPresentation(TrackPresentationSpec{
      .sortBy = {{TrackSortField::RecordingDate, false}},
    });
    checkOrder(projection, {nextDay, full, month, year, older, absent});

    // RecordingYear ties every precision in 1981 and retains source order.
    projection.setPresentation(TrackPresentationSpec{
      .sortBy = {{TrackSortField::RecordingYear, true}},
    });
    checkOrder(projection, {older, full, year, month, nextDay, absent});
    projection.setPresentation(TrackPresentationSpec{
      .sortBy = {{TrackSortField::RecordingYear, false}},
    });
    checkOrder(projection, {full, year, month, nextDay, older, absent});
  }

  TEST_CASE("TrackListProjection - classical presets separate Gould recording years within one compilation",
            "[runtime][unit][projection]")
  {
    auto fixture = TrackListProjectionFixture{};
    auto const newerSecond = addPerformanceTrack(fixture, "Compilation", {1981, 5, 12}, 2);
    auto const olderSecond = addPerformanceTrack(fixture, "Compilation", {1955, 6, 0}, 2);
    auto const newerFirst = addPerformanceTrack(fixture, "Compilation", {1981, 0, 0}, 1);
    auto const olderFirst = addPerformanceTrack(fixture, "Compilation", {1955, 0, 0}, 1);
    fixture.setupFiltered({{newerSecond, olderSecond, newerFirst, olderFirst}});
    auto projection = fixture.createProjection(ViewId{1});
    auto const subscription = projection.subscribe([](TrackListProjectionDeltaBatch const&) noexcept {});

    for (auto const* id : {"classical-composers", "classical-conductors", "classical-works"})
    {
      auto const* preset = builtinTrackPresentationPreset(id);
      REQUIRE(preset != nullptr);
      projection.setPresentation(preset->spec);
      checkOrder(projection, {olderFirst, olderSecond, newerFirst, newerSecond});
      CHECK(projection.groupCount() == 1);
      CHECK(std::ranges::contains(projection.presentation().visibleFields, TrackField::RecordingDate));
    }

    auto const groupBefore = projection.groupAt(0);
    CHECK(trackGroupHeadingText(groupBefore.heading.primary) == "Goldberg Variations, BWV 988");
    CHECK(trackGroupHeadingText(groupBefore.heading.secondary) == "J. S. Bach");
    auto descending = projection.presentation();
    descending.sortBy = {{TrackSortField::RecordingDate, false}};
    projection.setPresentation(descending);
    REQUIRE(projection.groupCount() == 1);
    CHECK(projection.groupAt(0).heading == groupBefore.heading);
    CHECK(projection.groupAt(0).rows.count == 4);
    checkOrder(projection, {newerSecond, newerFirst, olderSecond, olderFirst});
  }

  TEST_CASE("TrackListProjection - same-year sessions and precisions stay album-intact in classical presets",
            "[runtime][unit][projection]")
  {
    auto fixture = TrackListProjectionFixture{};
    auto const betaSecond = addPerformanceTrack(fixture, "Beta", {1981, 4, 15}, 2);
    auto const alphaSecond = addPerformanceTrack(fixture, "Alpha", {1981, 5, 1}, 2);
    auto const betaFirst = addPerformanceTrack(fixture, "Beta", {1981, 4, 1}, 1);
    auto const alphaFirst = addPerformanceTrack(fixture, "Alpha", {1981, 0, 0}, 1);
    fixture.setupFiltered({{betaSecond, alphaSecond, betaFirst, alphaFirst}});
    auto projection = fixture.createProjection(ViewId{1});
    auto const subscription = projection.subscribe([](TrackListProjectionDeltaBatch const&) noexcept {});

    for (auto const* id : {"classical-composers", "classical-conductors", "classical-works"})
    {
      auto const* preset = builtinTrackPresentationPreset(id);
      REQUIRE(preset != nullptr);
      projection.setPresentation(preset->spec);
      checkOrder(projection, {alphaFirst, alphaSecond, betaFirst, betaSecond});
    }
  }

  TEST_CASE("TrackListProjection - unknown recording years preserve the former release-year album movement order",
            "[runtime][unit][projection]")
  {
    auto fixture = TrackListProjectionFixture{};
    auto const later = addPerformanceTrack(fixture, "Alpha", {}, 1, 2001);
    auto const beta = addPerformanceTrack(fixture, "Beta", {}, 1, 2000);
    auto const alphaSecond = addPerformanceTrack(fixture, "Alpha", {}, 2, 2000);
    auto const alphaFirst = addPerformanceTrack(fixture, "Alpha", {}, 1, 2000);
    fixture.setupFiltered({{later, beta, alphaSecond, alphaFirst}});
    auto projection = fixture.createProjection(ViewId{1});
    auto const subscription = projection.subscribe([](TrackListProjectionDeltaBatch const&) noexcept {});

    for (auto const* id : {"classical-composers", "classical-conductors", "classical-works"})
    {
      auto const* preset = builtinTrackPresentationPreset(id);
      REQUIRE(preset != nullptr);
      projection.setPresentation(preset->spec);
      checkOrder(projection, {alphaFirst, alphaSecond, beta, later});
      auto const heading = projection.groupAt(0).heading;
      auto former = preset->spec;
      std::erase_if(
        former.sortBy, [](TrackSortTerm const& term) { return term.field == TrackSortField::RecordingYear; });
      projection.setPresentation(former);
      checkOrder(projection, {alphaFirst, alphaSecond, beta, later});
      CHECK(projection.groupAt(0).heading == heading);
    }
  }

  TEST_CASE("TrackListProjection - release-year zero retains ordinary numeric ordering", "[runtime][unit][projection]")
  {
    auto fixture = TrackListProjectionFixture{};
    auto const unknown = addPerformanceTrack(fixture, "Compilation", {}, 1, 0);
    auto const known = addPerformanceTrack(fixture, "Compilation", {}, 1, 1981);
    fixture.setupFiltered({{known, unknown}});
    auto projection = fixture.createProjection(ViewId{1});
    auto const subscription = projection.subscribe([](TrackListProjectionDeltaBatch const&) noexcept {});
    projection.setPresentation(TrackPresentationSpec{.sortBy = {{TrackSortField::Year, true}}});
    checkOrder(projection, {unknown, known});
    projection.setPresentation(TrackPresentationSpec{.sortBy = {{TrackSortField::Year, false}}});
    checkOrder(projection, {known, unknown});
  }

  TEST_CASE("TrackListProjection - recording date and year mutations keep incremental order and absence last",
            "[runtime][unit][projection]")
  {
    auto fixture = TrackListProjectionFixture{};
    auto const absent = addPerformanceTrack(fixture, "Compilation", {}, 1);
    auto const full = addPerformanceTrack(fixture, "Compilation", {1981, 5, 12}, 1);
    auto const yearOnly = addPerformanceTrack(fixture, "Compilation", {1981, 0, 0}, 1);
    auto const month = addPerformanceTrack(fixture, "Compilation", {1981, 5, 0}, 1);
    auto const older = addPerformanceTrack(fixture, "Compilation", {1955, 6, 0}, 1);
    auto const nextDay = addPerformanceTrack(fixture, "Compilation", {1981, 5, 13}, 1);
    fixture.setupFiltered({{absent, full, yearOnly, month, older, nextDay}});
    auto projection = fixture.createProjection(ViewId{1});
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto const subscription = projection.subscribe([&batches](TrackListProjectionDeltaBatch const& batch) noexcept
                                                   { batches.push_back(batch); });
    auto before = ProjectionCounts{};

    // The fixture source is the public delivery seam. The filtered smart list
    // forwards that update; the projection must not be rebuilt to observe it.
    auto deliver = [&](TrackId const id, library::RecordingDate const date)
    {
      fixture.libraryFixture.updateTrack(id, [date](library::test::TrackSpec& spec) { spec.recordingDate = date; });
      batches.clear();
      before = ::ao::rt::detail::RuntimeOperationProbe::counts(projection);
      fixture.source.update(id);
    };
    auto expect = [&](std::vector<TrackId> const& expected, ExpectedProjectionDelta const delta)
    { expectIncrementalDateOrder(fixture, projection, batches, before, expected, delta); };

    SECTION("ascending date")
    {
      projection.setPresentation(TrackPresentationSpec{.sortBy = {{TrackSortField::RecordingDate, true}}});
      batches.clear();
      checkOrder(projection, {older, yearOnly, month, full, nextDay, absent});

      // Replacement of the complete date moves that row ahead of 1955.
      deliver(full, {.year = 1954, .month = 1, .day = 1});
      expect({full, older, yearOnly, month, nextDay, absent}, {.moved = true, .from = 3, .to = 0});

      // Adding month and day precision moves 1981 between 1981-05 and 1981-05-13.
      deliver(yearOnly, {.year = 1981, .month = 5, .day = 1});
      expect({full, older, month, yearOnly, nextDay, absent}, {.moved = true, .from = 2, .to = 3});

      // Clearing keeps every absent date last. The original absent row has the
      // earlier source rank, so it stays ahead of the newly cleared row.
      deliver(nextDay, {});
      expect({full, older, month, yearOnly, absent, nextDay}, {.moved = true, .from = 4, .to = 5});
    }

    SECTION("descending date")
    {
      projection.setPresentation(TrackPresentationSpec{.sortBy = {{TrackSortField::RecordingDate, false}}});
      batches.clear();
      checkOrder(projection, {nextDay, full, month, yearOnly, older, absent});

      deliver(full, {.year = 1954, .month = 1, .day = 1});
      expect({nextDay, month, yearOnly, older, full, absent}, {.moved = true, .from = 1, .to = 4});

      deliver(yearOnly, {.year = 1981, .month = 5, .day = 1});
      expect({nextDay, yearOnly, month, older, full, absent}, {.moved = true, .from = 2, .to = 1});

      deliver(nextDay, {});
      expect({yearOnly, month, older, full, absent, nextDay}, {.moved = true, .from = 0, .to = 5});
    }

    SECTION("ascending year")
    {
      projection.setPresentation(TrackPresentationSpec{.sortBy = {{TrackSortField::RecordingYear, true}}});
      batches.clear();
      checkOrder(projection, {older, full, yearOnly, month, nextDay, absent});

      deliver(full, {.year = 1954, .month = 1, .day = 1});
      expect({full, older, yearOnly, month, nextDay, absent}, {.moved = true, .from = 1, .to = 0});

      // Month precision does not change the year key, so the row stays put.
      deliver(yearOnly, {.year = 1981, .month = 5, .day = 1});
      expect({full, older, yearOnly, month, nextDay, absent}, {.moved = false, .from = 2});

      deliver(nextDay, {});
      expect({full, older, yearOnly, month, absent, nextDay}, {.moved = true, .from = 4, .to = 5});
    }

    SECTION("descending year")
    {
      projection.setPresentation(TrackPresentationSpec{.sortBy = {{TrackSortField::RecordingYear, false}}});
      batches.clear();
      checkOrder(projection, {full, yearOnly, month, nextDay, older, absent});

      deliver(full, {.year = 1954, .month = 1, .day = 1});
      expect({yearOnly, month, nextDay, older, full, absent}, {.moved = true, .from = 0, .to = 4});

      deliver(yearOnly, {.year = 1981, .month = 5, .day = 1});
      expect({yearOnly, month, nextDay, older, full, absent}, {.moved = false, .from = 0});

      deliver(nextDay, {});
      expect({yearOnly, month, older, full, absent, nextDay}, {.moved = true, .from = 2, .to = 5});
    }
  }

  TEST_CASE("TrackListProjection - title sort keeps title order when a recording date is visible",
            "[runtime][unit][projection]")
  {
    auto fixture = TrackListProjectionFixture{};
    auto const zed =
      fixture.libraryFixture.addTrack(library::test::TrackSpec{.title = "Z", .recordingDate = {1955, 0, 0}});
    auto const alpha =
      fixture.libraryFixture.addTrack(library::test::TrackSpec{.title = "A", .recordingDate = {1981, 5, 12}});
    fixture.setupFiltered({{zed, alpha}});
    auto projection = fixture.createProjection(ViewId{1});
    auto const subscription = projection.subscribe([](TrackListProjectionDeltaBatch const&) noexcept {});
    projection.setPresentation(TrackPresentationSpec{
      .sortBy = {{TrackSortField::Title, true}},
      .visibleFields = {TrackField::Title, TrackField::RecordingDate},
    });

    // Order and the requested visible fields are the observable contract. No
    // approved load-count seam records cold reads, so this case does not prove
    // the title sort avoided opening cold metadata.
    auto const presentation = projection.presentation();
    REQUIRE(presentation.sortBy.size() == 1);
    CHECK(presentation.sortBy.front().field == TrackSortField::Title);
    CHECK(presentation.sortBy.front().ascending);
    CHECK(presentation.visibleFields == std::vector{TrackField::Title, TrackField::RecordingDate});
    checkOrder(projection, {alpha, zed});
  }
} // namespace ao::rt::test
