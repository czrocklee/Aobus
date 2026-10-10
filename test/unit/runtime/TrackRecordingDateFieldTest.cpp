// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/TrackFieldReaderInternal.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/FileTimestamp.h>
#include <ao/library/Credits.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Field.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace ao::rt::test
{
  TEST_CASE("TrackField - recording date appends stable typed field and sort identifiers",
            "[runtime][unit][trackfield]")
  {
    static_assert(static_cast<std::size_t>(TrackField::Quality) == 30);
    static_assert(static_cast<std::size_t>(TrackField::RecordingDate) == 31);
    static_assert(static_cast<std::size_t>(TrackSortField::Duration) == 14);
    static_assert(static_cast<std::size_t>(TrackSortField::RecordingYear) == 15);
    static_assert(static_cast<std::size_t>(TrackSortField::RecordingDate) == 16);
    static_assert(std::variant_size_v<TrackFieldRawValue> == 8);

    auto const* definition = trackFieldDefinition(TrackField::RecordingDate);
    REQUIRE(definition != nullptr);
    CHECK(definition->id == "recording-date");
    CHECK(definition->category == TrackFieldCategory::Metadata);
    CHECK(definition->valueKind == TrackFieldValueKind::RecordingDate);
    CHECK(definition->presentable);
    CHECK(definition->editable);
    CHECK(definition->sortable);
    CHECK_FALSE(definition->groupable);
    CHECK_FALSE(definition->synthetic);
    CHECK_FALSE(definition->valueCompletion);
    CHECK(definition->optSortField == TrackSortField::RecordingDate);
    CHECK_FALSE(definition->optGroupKey);
    CHECK(trackFieldFromId("recording-date") == TrackField::RecordingDate);
    CHECK(trackSortFieldFromId("recording-year") == TrackSortField::RecordingYear);
    CHECK(trackSortFieldFromId("recording-date") == TrackSortField::RecordingDate);
    CHECK(trackFieldQueryField(TrackField::RecordingDate) == query::Field::RecordingDate);
    CHECK(trackFieldFromQueryField(query::Field::RecordingDate) == TrackField::RecordingDate);
    CHECK(trackFieldFilterExpressionVariable(TrackField::RecordingDate) == "$recordingDate");
    CHECK_FALSE(supportsTrackFieldValueCompletion(TrackField::RecordingDate));
    CHECK(TrackFieldRawValue{std::in_place_type<FileTimestamp>}.index() == 6);
    CHECK(TrackFieldRawValue{std::in_place_type<library::RecordingDate>}.index() == 7);
  }

  TEST_CASE("TrackFieldReader - recording date retains exact precision and represents absence as monostate",
            "[runtime][unit][trackfield]")
  {
    auto fixture = MusicLibraryFixture{};
    auto const dates = std::to_array<library::RecordingDate>({{}, {1981, 0, 0}, {1981, 5, 0}, {1981, 5, 12}});

    for (auto const date : dates)
    {
      auto const id = fixture.addTrack(library::test::TrackSpec{.recordingDate = date});
      auto transaction = fixture.library().readTransaction();
      auto reader = fixture.library().tracks().reader(transaction);
      auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Cold);
      REQUIRE(optView);
      auto const raw =
        readTrackFieldRawValue(TrackField::RecordingDate, *optView, fixture.library().dictionary(), nullptr);

      if (date.isPresent())
      {
        auto const* stored = std::get_if<library::RecordingDate>(&raw);
        REQUIRE(stored != nullptr);
        CHECK(*stored == date);
      }
      else
      {
        CHECK(std::holds_alternative<std::monostate>(raw));
      }
    }

    CHECK(TrackFieldRawValue{library::RecordingDate{1981, 0, 0}} !=
          TrackFieldRawValue{library::RecordingDate{1981, 5, 12}});
  }

  TEST_CASE("TrackFieldReader - work metadata and performance projections retain independent values",
            "[runtime][unit][trackfield]")
  {
    auto fixture = MusicLibraryFixture{};
    auto const id = fixture.addTrack(library::test::TrackSpec{
      .title = "Variation",
      .work = "Goldberg Variations, BWV 988",
      .movement = "Variation 1",
      .recordingDate = {1955, 6, 0},
      .credits = {{.name = "Conductor", .kind = library::CreditKind::Conductor},
                  {.name = "Ensemble", .kind = library::CreditKind::Ensemble},
                  {.name = "Glenn Gould", .kind = library::CreditKind::Soloist}},
      .movementNumber = 1,
      .movementTotal = 30,
    });
    auto transaction = fixture.library().readTransaction();
    auto reader = fixture.library().tracks().reader(transaction);
    auto const optCold = reader.get(id, library::TrackStore::Reader::LoadMode::Cold);
    REQUIRE(optCold);
    auto const read = [&](TrackField const field)
    { return readTrackFieldRawValue(field, *optCold, fixture.library().dictionary(), nullptr); };
    CHECK(std::get<std::string>(read(TrackField::Work)) == "Goldberg Variations, BWV 988");
    CHECK(std::get<std::string>(read(TrackField::Movement)) == "Variation 1");
    CHECK(std::get<std::string>(read(TrackField::Conductor)) == "Conductor");
    CHECK(std::get<std::string>(read(TrackField::Ensemble)) == "Ensemble");
    CHECK(std::get<std::string>(read(TrackField::Soloist)) == "Glenn Gould");
    CHECK(std::get<std::uint16_t>(read(TrackField::MovementNumber)) == 1);
    CHECK(std::get<std::uint16_t>(read(TrackField::MovementTotal)) == 30);
    CHECK(std::get<library::RecordingDate>(read(TrackField::RecordingDate)) == library::RecordingDate{1955, 6, 0});

    auto const optHot = reader.get(id, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optHot);
    CHECK_FALSE(optHot->isColdValid());
    CHECK(std::get<std::string>(readTrackFieldRawValue(
            TrackField::Title, *optHot, fixture.library().dictionary(), nullptr)) == "Variation");
  }

  TEST_CASE("TrackDetailProjection - recording date aggregation distinguishes precision and absence",
            "[runtime][unit][projection]")
  {
    auto fixture = ViewServiceFixture{};
    auto const year = fixture.addTrack(library::test::TrackSpec{.recordingDate = {1981, 0, 0}});
    auto const sameYear = fixture.addTrack(library::test::TrackSpec{.recordingDate = {1981, 0, 0}});
    auto const precise = fixture.addTrack(library::test::TrackSpec{.recordingDate = {1981, 5, 12}});
    auto const absent = fixture.addTrack(library::test::TrackSpec{});
    auto const alsoAbsent = fixture.addTrack(library::test::TrackSpec{});

    SECTION("equal partial dates are a common exact value")
    {
      auto const projectionPtr = fixture.workspace.detailProjection(ExplicitSelectionTarget{{year, sameYear}});
      auto const snapshot = projectionPtr->snapshot();
      auto const& aggregate = trackFieldArrayAt(snapshot.fields, TrackField::RecordingDate);
      CHECK_FALSE(aggregate.mixed);
      REQUIRE(aggregate.optValue);
      CHECK(std::get<library::RecordingDate>(*aggregate.optValue) == library::RecordingDate{1981, 0, 0});
    }

    SECTION("same year at different precisions is mixed")
    {
      auto const projectionPtr = fixture.workspace.detailProjection(ExplicitSelectionTarget{{year, precise}});
      auto const snapshot = projectionPtr->snapshot();
      auto const& aggregate = trackFieldArrayAt(snapshot.fields, TrackField::RecordingDate);
      CHECK(aggregate.mixed);
      CHECK_FALSE(aggregate.optValue);
    }

    SECTION("absence and a present date are mixed")
    {
      auto const projectionPtr = fixture.workspace.detailProjection(ExplicitSelectionTarget{{absent, year}});
      auto const snapshot = projectionPtr->snapshot();
      auto const& aggregate = trackFieldArrayAt(snapshot.fields, TrackField::RecordingDate);
      CHECK(aggregate.mixed);
      CHECK_FALSE(aggregate.optValue);
    }

    SECTION("all absent dates are a common monostate")
    {
      auto const projectionPtr = fixture.workspace.detailProjection(ExplicitSelectionTarget{{absent, alsoAbsent}});
      auto const snapshot = projectionPtr->snapshot();
      auto const& aggregate = trackFieldArrayAt(snapshot.fields, TrackField::RecordingDate);
      CHECK_FALSE(aggregate.mixed);
      REQUIRE(aggregate.optValue);
      CHECK(std::holds_alternative<std::monostate>(*aggregate.optValue));
    }
  }
} // namespace ao::rt::test
