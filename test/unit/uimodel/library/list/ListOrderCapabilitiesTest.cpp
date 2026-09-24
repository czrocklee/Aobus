// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/list/ListOrder.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    void checkDisabled(ListOrderCapabilityState const& state)
    {
      CHECK_FALSE(state.canAuthorOrder);
      CHECK_FALSE(state.canGapMove);
      CHECK_FALSE(state.canRelativeMove);
      CHECK_FALSE(state.canAbsoluteMove);
      CHECK_FALSE(state.canResetOrder);
      CHECK_FALSE(state.canForgetHiddenPositions);
    }

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

  TEST_CASE("ListOrder - capability follows structure rather than presentation id", "[uimodel][unit][list][list-order]")
  {
    auto input = eligibleInput();
    auto const state = describeListOrderCapabilities(ao::test::englishMessageCatalog(), input);

    CHECK(state.canAuthorOrder);
    CHECK(state.canGapMove);
    CHECK(state.canRelativeMove);
    CHECK(state.canAbsoluteMove);
    CHECK(state.canResetOrder);
    CHECK(state.canForgetHiddenPositions);
    CHECK(state.disabledReason.empty());

    input.presentation.id = std::string{rt::kListOrderTrackPresentationId};
    input.presentation.sortBy = {rt::TrackSortTerm{.field = rt::TrackSortField::Title, .ascending = true}};
    auto const sorted = describeListOrderCapabilities(ao::test::englishMessageCatalog(), input);
    checkDisabled(sorted);
  }

  TEST_CASE("ListOrder - quick filter keeps only absolute moves", "[uimodel][unit][list][list-order]")
  {
    auto input = eligibleInput();
    input.quickFilterExpression = "$year >= 2020";

    auto const state = describeListOrderCapabilities(ao::test::englishMessageCatalog(), input);

    CHECK(state.canAuthorOrder);
    CHECK_FALSE(state.canGapMove);
    CHECK_FALSE(state.canRelativeMove);
    CHECK(state.canAbsoluteMove);
    CHECK(state.canResetOrder);
    CHECK(state.canForgetHiddenPositions);
    CHECK(state.disabledReason.contains("Clear the quick filter"));
  }

  TEST_CASE("ListOrder - disabled reasons come from the injected locale",
            "[uimodel][unit][list][list-order][localization]")
  {
    auto input = eligibleInput();
    input.authoring.state = rt::LibraryAuthoringState::Maintenance;

    auto const state = describeListOrderCapabilities(ao::test::messageCatalog("de-AT"), input);

    checkDisabled(state);
    CHECK(state.disabledReason ==
          "Die Bibliothek ist beschäftigt. Die manuelle Reihenfolge ist nach Abschluss der Wartung wieder verfügbar.");
  }

  TEST_CASE("ListOrder - rejects virtual, grouped, unavailable, and erroneous sources",
            "[uimodel][unit][list][list-order]")
  {
    auto input = eligibleInput();
    auto expectedReason = std::string_view{};

    SECTION("All Tracks")
    {
      input.listId = rt::kAllTracksListId;
      expectedReason = "saved Lists";
    }

    SECTION("grouped")
    {
      input.presentation.groupBy = rt::TrackGroupKey::Album;
      expectedReason = "flat unsorted";
    }

    SECTION("maintenance")
    {
      input.authoring.state = rt::LibraryAuthoringState::Maintenance;
      expectedReason = "Library is busy";
    }

    SECTION("source gone")
    {
      input.sourceLive = false;
      expectedReason = "no longer available";
    }

    SECTION("filter error")
    {
      input.sourceHasError = true;
      expectedReason = "Fix the List or quick-filter expression";
    }

    SECTION("virtual source takes precedence over maintenance")
    {
      input.listId = rt::kAllTracksListId;
      input.authoring.state = rt::LibraryAuthoringState::Maintenance;
      expectedReason = "saved Lists";
    }

    SECTION("maintenance takes precedence over unavailable source")
    {
      input.authoring.state = rt::LibraryAuthoringState::Maintenance;
      input.sourceLive = false;
      expectedReason = "Library is busy";
    }

    SECTION("source error takes precedence over grouped presentation")
    {
      input.sourceHasError = true;
      input.presentation.groupBy = rt::TrackGroupKey::Album;
      expectedReason = "Fix the List or quick-filter expression";
    }

    SECTION("grouped presentation takes precedence over quick filter")
    {
      input.presentation.groupBy = rt::TrackGroupKey::Album;
      input.quickFilterExpression = "$year >= 2020";
      expectedReason = "flat unsorted";
    }

    auto const state = describeListOrderCapabilities(ao::test::englishMessageCatalog(), input);
    checkDisabled(state);
    REQUIRE_FALSE(expectedReason.empty());
    CHECK(state.disabledReason.contains(expectedReason));
  }

  TEST_CASE("ListOrder - drag selection follows effective order", "[uimodel][unit][list][list-order]")
  {
    auto const effective = std::array{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}};

    CHECK(listOrderDragSelection(TrackId{3}, std::array{TrackId{4}, TrackId{2}}, effective) == std::vector{TrackId{3}});
    CHECK(listOrderDragSelection(TrackId{2}, std::array{TrackId{4}, TrackId{2}}, effective) ==
          std::vector{TrackId{2}, TrackId{4}});
    CHECK(listOrderDragSelection(TrackId{99}, std::array{TrackId{99}}, effective).empty());
  }

  TEST_CASE("ListOrder - drop gaps normalize around the dragged selection", "[uimodel][unit][list][list-order]")
  {
    auto const effective = std::array{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}};
    auto const selected = std::array{TrackId{2}, TrackId{3}};

    auto const beforeFirstRes = listOrderAnchorForGap(effective, selected, 0);
    REQUIRE(beforeFirstRes);
    REQUIRE(*beforeFirstRes);
    CHECK(**beforeFirstRes == TrackId{1});

    for (std::size_t gapIndex = 1; gapIndex < effective.size(); ++gapIndex)
    {
      auto const beforeFourthRes = listOrderAnchorForGap(effective, selected, gapIndex);
      REQUIRE(beforeFourthRes);
      REQUIRE(*beforeFourthRes);
      CHECK(**beforeFourthRes == TrackId{4});
    }

    auto const atEndRes = listOrderAnchorForGap(effective, selected, 4);
    REQUIRE(atEndRes);
    CHECK_FALSE(atEndRes->has_value());

    auto const outsideRes = listOrderAnchorForGap(effective, selected, 5);
    REQUIRE_FALSE(outsideRes);
    CHECK(outsideRes.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("listOrderAnchorForGap rejects invalid track identities", "[uimodel][unit][list][list-order]")
  {
    SECTION("effective sequence")
    {
      auto const effective = std::array{TrackId{1}, kInvalidTrackId, TrackId{3}};
      auto const selected = std::array{TrackId{1}};

      auto const res = listOrderAnchorForGap(effective, selected, 1);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
    }

    SECTION("selected span")
    {
      auto const effective = std::array{TrackId{1}, TrackId{2}, TrackId{3}};
      auto const selected = std::array{kInvalidTrackId};

      auto const res = listOrderAnchorForGap(effective, selected, 1);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
    }
  }
} // namespace ao::uimodel::test
