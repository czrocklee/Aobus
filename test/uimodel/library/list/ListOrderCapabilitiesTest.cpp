// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/MessageCatalogTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/list/ListOrderOutcome.h>

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
            .presetId = rt::CustomPresetId{1},
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

  TEST_CASE("ListOrder - capability follows structure rather than presentation id")
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

    input.presentation.presetId = rt::BuiltinPreset::ListOrder;

    SECTION("preset sort prevents order authoring")
    {
      input.presentation.sortBy = {{.field = rt::TrackSortField::Title, .ascending = true}};
    }

    auto const sorted = describeListOrderCapabilities(ao::test::englishMessageCatalog(), input);
    checkDisabled(sorted);
    CHECK(sorted.disabledReason.contains("flat unsorted"));
  }

  TEST_CASE("ListOrder - a header override asks to choose the current view again")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto input = eligibleInput();
    input.presentation.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::Title, .ascending = true};
    auto const expected = ao::i18n::requiredText(catalog, ao::i18n::MessageId::ListOrderChooseCurrentView);

    SECTION("custom flat preset")
    {
      auto const state = describeListOrderCapabilities(catalog, input);

      checkDisabled(state);
      CHECK(state.disabledReason == expected);
    }

    SECTION("built-in Manual Order uses the same recovery")
    {
      input.presentation.presetId = rt::BuiltinPreset::ListOrder;
      auto const state = describeListOrderCapabilities(catalog, input);

      checkDisabled(state);
      CHECK(state.disabledReason == expected);
    }

    SECTION("loaded view projects the same reason")
    {
      auto const view = rt::TrackListViewState{.listId = input.listId, .presentation = input.presentation};
      auto const state = describeViewListOrderCapabilities(catalog, view, rt::TrackSourceState::Live, input.authoring);

      checkDisabled(state);
      CHECK(state.disabledReason == expected);
    }
  }

  TEST_CASE("ListOrder - earlier blocks take precedence over a header override")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto input = eligibleInput();
    input.presentation.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::Artist, .ascending = false};
    auto expected = ao::i18n::MessageId::ListOrderChooseCurrentView;

    SECTION("virtual source")
    {
      input.listId = rt::kAllTracksListId;
      expected = ao::i18n::MessageId::ListOrderSavedListsOnly;
    }

    SECTION("maintenance")
    {
      input.authoring.state = rt::LibraryAuthoringState::Maintenance;
      expected = ao::i18n::MessageId::ListOrderLibraryBusy;
    }

    SECTION("source unavailable")
    {
      input.sourceLive = false;
      expected = ao::i18n::MessageId::ListOrderListUnavailable;
    }

    SECTION("filter error")
    {
      input.sourceHasError = true;
      expected = ao::i18n::MessageId::ListOrderFixFilter;
    }

    SECTION("grouped presentation")
    {
      input.presentation.groupBy = rt::TrackGroupKey::Album;
      expected = ao::i18n::MessageId::ListOrderChooseFlatPresentation;
    }

    SECTION("base sort")
    {
      input.presentation.sortBy = {{.field = rt::TrackSortField::Title, .ascending = true}};
      expected = ao::i18n::MessageId::ListOrderChooseFlatPresentation;
    }

    SECTION("quick filter does not replace the header-override recovery")
    {
      input.quickFilterExpression = "$year >= 2020";
      expected = ao::i18n::MessageId::ListOrderChooseCurrentView;
    }

    auto const state = describeListOrderCapabilities(catalog, input);
    checkDisabled(state);
    CHECK(state.disabledReason == ao::i18n::requiredText(catalog, expected));
  }

  TEST_CASE("ListOrder - quick filter keeps only absolute moves")
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

  TEST_CASE("ListOrder - disabled reasons come from the injected locale")
  {
    auto input = eligibleInput();
    input.authoring.state = rt::LibraryAuthoringState::Maintenance;

    auto const state = describeListOrderCapabilities(ao::test::messageCatalog("de-AT"), input);

    checkDisabled(state);
    CHECK(state.disabledReason ==
          "Die Bibliothek ist beschäftigt. Die manuelle Reihenfolge ist nach Abschluss der Wartung wieder verfügbar.");
  }

  TEST_CASE("ListOrder - rejects virtual, grouped, unavailable, and erroneous sources")
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

  TEST_CASE("ListOrder - loaded view details determine which order commands remain available")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto view = rt::TrackListViewState{.listId = ListId{42}, .filterExpression = "true"};
    auto availability = rt::LibraryAuthoringAvailability{};
    auto sourceState = rt::TrackSourceState::Live;

    SECTION("live quick-filtered view")
    {
      auto const state = describeViewListOrderCapabilities(textCatalog, view, sourceState, availability);

      CHECK(state.canAuthorOrder);
      CHECK_FALSE(state.canGapMove);
      CHECK_FALSE(state.canRelativeMove);
      CHECK(state.canAbsoluteMove);
      CHECK(state.canResetOrder);
      CHECK(state.canForgetHiddenPositions);
    }

    SECTION("filter error on a live view")
    {
      view.filterExpression = "(";
      view.optFilterError = Error{.code = Error::Code::InvalidInput, .message = "Invalid expression"};
      auto const state = describeViewListOrderCapabilities(textCatalog, view, sourceState, availability);

      checkDisabled(state);
      CHECK(state.disabledReason == "Fix the List or quick-filter expression before changing its order.");
    }

    SECTION("invalidated source")
    {
      sourceState = rt::TrackSourceState::Invalidated;
      auto const state = describeViewListOrderCapabilities(textCatalog, view, sourceState, availability);

      checkDisabled(state);
      CHECK(state.disabledReason == "This List is no longer available.");
    }

    SECTION("authoring under maintenance")
    {
      availability.state = rt::LibraryAuthoringState::Maintenance;
      auto const state = describeViewListOrderCapabilities(textCatalog, view, sourceState, availability);

      checkDisabled(state);
      CHECK(state.disabledReason == "Library is busy. Manual ordering will be available when maintenance finishes.");
    }
  }

  TEST_CASE("ListOrder - applied outcomes format the count from the typed action reply")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const move = rt::AuthoringResult<rt::MoveListOrderReply>{
      .status = rt::AuthoringStatus::Applied,
      .reply = {.selectedTrackIds = {TrackId{1}, TrackId{2}}},
    };
    auto const reset = rt::AuthoringResult<rt::ResetListOrderReply>{
      .status = rt::AuthoringStatus::Applied,
      .reply = {.forgottenPositionCount = 3},
    };
    auto const forgetHidden = rt::AuthoringResult<rt::ForgetHiddenListOrderReply>{
      .status = rt::AuthoringStatus::Applied,
      .reply = {.forgottenPositionCount = 1},
    };

    CHECK(formatListOrderOutcome(textCatalog, move) == "Moved 2 tracks in Manual Order.");
    CHECK(formatListOrderOutcome(textCatalog, reset) == "Reset Manual Order and forgot 3 saved positions.");
    CHECK(formatListOrderOutcome(textCatalog, forgetHidden) == "Forgot 1 hidden saved position.");
  }

  TEST_CASE("ListOrder - unchanged and refused outcomes report guidance without a count")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    using rt::AuthoringStatus;

    CHECK(formatListOrderOutcome(
            textCatalog, rt::AuthoringResult<rt::MoveListOrderReply>{.status = AuthoringStatus::NoOp}) ==
          "Order unchanged.");
    CHECK(formatListOrderOutcome(
            textCatalog, rt::AuthoringResult<rt::MoveListOrderReply>{.status = AuthoringStatus::Busy}) ==
          "Library is busy. Manual ordering will be available when maintenance finishes.");
    CHECK(formatListOrderOutcome(
            textCatalog, rt::AuthoringResult<rt::ResetListOrderReply>{.status = AuthoringStatus::Stale}) ==
          "The List changed. Start the order action again.");
    CHECK(formatListOrderOutcome(
            textCatalog, rt::AuthoringResult<rt::ForgetHiddenListOrderReply>{.status = AuthoringStatus::Unavailable}) ==
          "Library editing is currently unavailable.");
  }

  TEST_CASE("ListOrder - drag selection follows effective order")
  {
    auto const effective = std::array{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}};

    CHECK(listOrderDragSelection(TrackId{3}, std::array{TrackId{4}, TrackId{2}}, effective) == std::vector{TrackId{3}});
    CHECK(listOrderDragSelection(TrackId{2}, std::array{TrackId{4}, TrackId{2}}, effective) ==
          std::vector{TrackId{2}, TrackId{4}});
    CHECK(listOrderDragSelection(TrackId{99}, std::array{TrackId{99}}, effective).empty());
  }

  TEST_CASE("ListOrder - drop gaps normalize around the dragged selection")
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

  TEST_CASE("listOrderAnchorForGap rejects invalid track identities")
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
