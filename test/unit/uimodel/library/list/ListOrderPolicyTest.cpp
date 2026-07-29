// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/list/ListOrderPolicy.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    ListOrderCapabilityInput eligibleInput()
    {
      return ListOrderCapabilityInput{
        .listId = ListId{42},
        .presentation =
          rt::TrackPresentationSpec{
            .id = "custom-flat-order",
            .groupBy = rt::TrackGroupKey::None,
            .sortBy = {},
          },
        .quickFilterExpression = "",
        .sourceLive = true,
        .sourceHasError = false,
        .authoring =
          rt::LibraryAuthoringAvailability{
            .state = rt::LibraryAuthoringState::Available,
            .runtimeInstanceId = 7,
            .libraryRevision = 9,
          },
      };
    }
  } // namespace

  TEST_CASE("ListOrderPolicy - capability follows structure rather than presentation id",
            "[uimodel][unit][list][list-order]")
  {
    auto input = eligibleInput();
    auto const state = describeListOrderCapabilities(input);

    CHECK(state.canAuthorOrder);
    CHECK(state.canGapMove);
    CHECK(state.canRelativeMove);
    CHECK(state.canAbsoluteMove);
    CHECK(state.canResetOrder);
    CHECK(state.canForgetHiddenPositions);
    CHECK(state.disabledReason.empty());

    input.presentation.id = std::string{rt::kListOrderTrackPresentationId};
    input.presentation.sortBy = {rt::TrackSortTerm{.field = rt::TrackSortField::Title, .ascending = true}};
    auto const sorted = describeListOrderCapabilities(input);
    CHECK_FALSE(sorted.canAuthorOrder);
    CHECK_FALSE(sorted.canAbsoluteMove);
  }

  TEST_CASE("ListOrderPolicy - quick filter keeps only absolute moves", "[uimodel][unit][list][list-order]")
  {
    auto input = eligibleInput();
    input.quickFilterExpression = "$year >= 2020";

    auto const state = describeListOrderCapabilities(input);

    CHECK(state.canAuthorOrder);
    CHECK_FALSE(state.canGapMove);
    CHECK_FALSE(state.canRelativeMove);
    CHECK(state.canAbsoluteMove);
    CHECK(state.canResetOrder);
    CHECK(state.canForgetHiddenPositions);
    CHECK(state.disabledReason.contains("Clear the quick filter"));
  }

  TEST_CASE("ListOrderPolicy - rejects virtual, grouped, unavailable, and erroneous sources",
            "[uimodel][unit][list][list-order]")
  {
    auto input = eligibleInput();

    SECTION("All Tracks")
    {
      input.listId = rt::kAllTracksListId;
      auto const state = describeListOrderCapabilities(input);
      CHECK_FALSE(state.canAuthorOrder);
      CHECK(state.disabledReason.contains("saved Lists"));
    }

    SECTION("grouped")
    {
      input.presentation.groupBy = rt::TrackGroupKey::Album;
      auto const state = describeListOrderCapabilities(input);
      CHECK_FALSE(state.canAuthorOrder);
      CHECK(state.disabledReason.contains("flat unsorted"));
    }

    SECTION("maintenance")
    {
      input.authoring.state = rt::LibraryAuthoringState::Maintenance;
      input.authoring.maintenanceKind = rt::LibraryMaintenanceKind::ScanApply;
      auto const state = describeListOrderCapabilities(input);
      CHECK_FALSE(state.canAuthorOrder);
      CHECK(state.disabledReason.contains("Library is busy"));
    }

    SECTION("faulted")
    {
      input.authoring.state = rt::LibraryAuthoringState::Faulted;
      auto const state = describeListOrderCapabilities(input);
      CHECK_FALSE(state.canAuthorOrder);
      CHECK(state.disabledReason == "Library authoring is unavailable.");
    }

    SECTION("source gone")
    {
      input.sourceLive = false;
      auto const state = describeListOrderCapabilities(input);
      CHECK_FALSE(state.canAuthorOrder);
      CHECK(state.disabledReason.contains("no longer available"));
    }

    SECTION("filter error")
    {
      input.sourceHasError = true;
      auto const state = describeListOrderCapabilities(input);
      CHECK_FALSE(state.canAuthorOrder);
      CHECK(state.disabledReason.contains("Fix the List or quick-filter expression"));
    }
  }

  TEST_CASE("ListOrderPolicy - drag selection follows effective order", "[uimodel][unit][list][list-order]")
  {
    auto const effective = std::array{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}};

    CHECK(listOrderDragSelection(TrackId{3}, std::array{TrackId{4}, TrackId{2}}, effective) == std::vector{TrackId{3}});
    CHECK(listOrderDragSelection(TrackId{2}, std::array{TrackId{4}, TrackId{2}}, effective) ==
          std::vector{TrackId{2}, TrackId{4}});
    CHECK(listOrderDragSelection(TrackId{99}, std::array{TrackId{99}}, effective).empty());
  }

  TEST_CASE("ListOrderPolicy - drop gaps normalize around the dragged selection", "[uimodel][unit][list][list-order]")
  {
    auto const effective = std::array{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}};
    auto const selected = std::array{TrackId{2}, TrackId{3}};

    auto const beforeFirst = listOrderAnchorForGap(effective, selected, 0);
    REQUIRE(beforeFirst);
    REQUIRE(*beforeFirst);
    CHECK(**beforeFirst == TrackId{1});

    for (std::size_t gapIndex = 1; gapIndex < effective.size(); ++gapIndex)
    {
      auto const beforeFourth = listOrderAnchorForGap(effective, selected, gapIndex);
      REQUIRE(beforeFourth);
      REQUIRE(*beforeFourth);
      CHECK(**beforeFourth == TrackId{4});
    }

    auto const atEnd = listOrderAnchorForGap(effective, selected, 4);
    REQUIRE(atEnd);
    CHECK_FALSE(atEnd->has_value());

    auto const outside = listOrderAnchorForGap(effective, selected, 5);
    REQUIRE_FALSE(outside);
    CHECK(outside.error().code == Error::Code::InvalidInput);
  }
} // namespace ao::uimodel::test
