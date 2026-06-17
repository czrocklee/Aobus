// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/list/SmartListEditorModel.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using namespace ao;

namespace ao::uimodel::list::test
{
  TEST_CASE("SmartListEditorModel - effective expression", "[unit][uimodel][list]")
  {
    SECTION("Display expression")
    {
      CHECK(SmartListEditorModel::displayExpression("") == "(none)");
      CHECK(SmartListEditorModel::displayExpression("$genre = 'Jazz'") == "$genre = 'Jazz'");
    }

    SECTION("Local expression only")
    {
      CHECK(SmartListEditorModel::composeEffectiveExpression("", "$artist = 'Queen'") == "$artist = 'Queen'");
    }

    SECTION("Parent and local expression")
    {
      auto const effective = SmartListEditorModel::composeEffectiveExpression("$year > 1970", "$artist = 'Queen'");
      CHECK(effective == "($year > 1970) and ($artist = 'Queen')");
    }

    SECTION("Parent only")
    {
      CHECK(SmartListEditorModel::composeEffectiveExpression("$year > 1970", "") == "$year > 1970");
    }
  }

  TEST_CASE("SmartListEditorModel - preview view state", "[unit][uimodel][list]")
  {
    SECTION("Preview source unavailable")
    {
      auto const state = SmartListEditorModel::previewState(SmartListPreviewInput{
        .name = "Library Picks",
        .localExpression = "$artist = 'Queen'",
        .hasPreviewSource = false,
        .hasError = false,
        .errorMessage = "",
        .matchCount = 0,
        .isAllTracks = false,
      });

      CHECK(state.status == SmartListStatus::InvalidExpression);
      CHECK(state.expressionValid == false);
      CHECK(state.previewVisible == false);
      CHECK(state.errorVisible == false);
      CHECK(state.canSubmit == false);
    }

    SECTION("Empty local expression shows full source")
    {
      auto const state = SmartListEditorModel::previewState(SmartListPreviewInput{
        .name = "Source Tracks",
        .localExpression = "",
        .hasPreviewSource = true,
        .hasError = false,
        .errorMessage = "",
        .matchCount = 4,
        .isAllTracks = false,
      });

      CHECK(state.status == SmartListStatus::Valid);
      CHECK(state.previewStatusText == "Showing all 4 source tracks");
      CHECK(state.expressionValid == true);
      CHECK(state.previewVisible == true);
      CHECK(state.errorVisible == false);
      CHECK(state.canSubmit == true);
    }

    SECTION("Empty all-tracks source has library wording")
    {
      auto const state = SmartListEditorModel::previewState(SmartListPreviewInput{
        .name = "Empty Library",
        .localExpression = "",
        .hasPreviewSource = true,
        .hasError = false,
        .errorMessage = "",
        .matchCount = 0,
        .isAllTracks = true,
      });

      CHECK(state.previewStatusText == "No tracks in library");
    }

    SECTION("Valid filter reports match count")
    {
      auto const small = SmartListEditorModel::previewStatusText(SmartListStatus::Valid, 3, true, false);
      auto const large = SmartListEditorModel::previewStatusText(SmartListStatus::Valid, 14, true, false);

      CHECK(SmartListEditorModel::previewStatusText(SmartListStatus::Valid, 0, true, false) == "No matches");
      CHECK(small == "Showing all 3 matches");
      CHECK(large == "Showing 10 of 14 matches");
    }

    SECTION("Empty source status is represented explicitly")
    {
      CHECK(SmartListEditorModel::dialogStatus(true, false) == SmartListStatus::EmptySource);
      CHECK(SmartListEditorModel::previewStatusText(SmartListStatus::EmptySource, 0, false, false) ==
            "No tracks in source");
    }

    SECTION("Unknown preview status falls back to empty text")
    {
      CHECK(SmartListEditorModel::previewStatusText(static_cast<SmartListStatus>(250), 2, true, false).empty());
    }

    SECTION("Invalid filter hides preview and exposes error")
    {
      auto const state = SmartListEditorModel::previewState(SmartListPreviewInput{
        .name = "Broken Filter",
        .localExpression = "$artist =",
        .hasPreviewSource = true,
        .hasError = true,
        .errorMessage = "expected value",
        .matchCount = 0,
        .isAllTracks = true,
      });

      CHECK(state.status == SmartListStatus::InvalidExpression);
      CHECK(state.previewStatusText == "Invalid filter");
      CHECK(state.queryInvalid == true);
      CHECK(state.errorVisible == true);
      CHECK(state.previewVisible == false);
      CHECK(state.expressionValid == false);
      CHECK(state.errorText == "Filter error: expected value");
      CHECK(state.canSubmit == false);
    }

    SECTION("Invalid empty filter keeps preview visible")
    {
      auto const state = SmartListEditorModel::previewState(SmartListPreviewInput{
        .name = "All Tracks",
        .localExpression = "",
        .hasPreviewSource = true,
        .hasError = true,
        .errorMessage = "ignored for empty filter",
        .matchCount = 5,
        .isAllTracks = true,
      });

      CHECK(state.queryInvalid == false);
      CHECK(state.errorVisible == false);
      CHECK(state.previewVisible == true);
      CHECK(state.expressionValid == true);
      CHECK(state.canSubmit == true);
    }
  }

  TEST_CASE("SmartListEditorModel - presentation selection", "[unit][uimodel][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();
    REQUIRE(presets.size() >= 2);

    SECTION("Dropdown index maps known ids after Auto")
    {
      auto const optId = std::optional<std::string>{std::string{presets[1].spec.id}};
      CHECK(SmartListEditorModel::presentationIndexForId(optId, presets) == 2);
      CHECK(SmartListEditorModel::presentationIndexForId(std::nullopt, presets) ==
            SmartListEditorModel::kAutoPresentationIndex);
      CHECK(SmartListEditorModel::presentationIndexForId(std::optional<std::string>{"unknown"}, presets) ==
            SmartListEditorModel::kAutoPresentationIndex);
    }

    SECTION("Manual selection resolves builtin id")
    {
      CHECK(SmartListEditorModel::resolvePresentationId(2, true, "$album = 'Kind of Blue'", presets, {}) ==
            presets[1].spec.id);
    }

    SECTION("Auto and invalid positions resolve to recommendation")
    {
      auto const autoId = SmartListEditorModel::resolvePresentationId(0, true, "$album = 'Kind of Blue'", presets, {});
      auto const invalidId =
        SmartListEditorModel::resolvePresentationId(999, false, "$album = 'Kind of Blue'", presets, {});

      CHECK(autoId == invalidId);
      CHECK_FALSE(autoId.empty());
    }

    SECTION("Out-of-range manual selection falls back to default")
    {
      CHECK(SmartListEditorModel::resolvePresentationId(presets.size() + 1, true, "$artist = 'Queen'", presets, {}) ==
            rt::kDefaultTrackPresentationId);
    }
  }

  TEST_CASE("SmartListEditorModel - SmartListStatus enum values", "[unit][uimodel][list]")
  {
    CHECK(static_cast<int>(SmartListStatus::Valid) == 1);
  }

  TEST_CASE("SmartListEditorModel - canSubmit", "[unit][uimodel][list]")
  {
    SECTION("Valid name and status")
    {
      CHECK(SmartListEditorModel::canSubmit("My Smart List", SmartListStatus::Valid) == true);
    }

    SECTION("Empty name should be rejected")
    {
      CHECK(SmartListEditorModel::canSubmit("", SmartListStatus::Valid) == false);
    }

    SECTION("Invalid expression should be rejected")
    {
      CHECK(SmartListEditorModel::canSubmit("My List", SmartListStatus::InvalidExpression) == false);
    }

    SECTION("Empty name with invalid expression should be rejected")
    {
      CHECK(SmartListEditorModel::canSubmit("", SmartListStatus::InvalidExpression) == false);
    }

    SECTION("Empty source is treated as valid name")
    {
      CHECK(SmartListEditorModel::canSubmit("My List", SmartListStatus::EmptySource) == true);
    }
  }

  TEST_CASE("SmartListEditorModel - createDraft", "[unit][uimodel][list]")
  {
    auto const parentListId = ListId{10};
    auto const editListId = ListId{42};
    auto const name = std::string{"My Smart List"};
    auto const description = std::string{"A description"};
    auto const expression = std::string{"$artist = 'Queen'"};

    SECTION("Full draft with all fields")
    {
      auto const draft = SmartListEditorModel::createDraft(parentListId, editListId, name, description, expression);

      CHECK(draft.kind == rt::LibraryMutationService::ListKind::Smart);
      CHECK(draft.parentId == parentListId);
      CHECK(draft.listId == editListId);
      CHECK(draft.name == name);
      CHECK(draft.description == description);
      CHECK(draft.expression == expression);
    }

    SECTION("Draft for creating (listId = kInvalidListId)")
    {
      auto const draft = SmartListEditorModel::createDraft(parentListId, kInvalidListId, name, description, expression);

      CHECK(draft.kind == rt::LibraryMutationService::ListKind::Smart);
      CHECK(draft.parentId == parentListId);
      CHECK(draft.listId == kInvalidListId);
      CHECK(draft.name == name);
      CHECK(draft.description == description);
      CHECK(draft.expression == expression);
    }

    SECTION("Draft with empty strings")
    {
      auto const draft = SmartListEditorModel::createDraft(kInvalidListId, kInvalidListId, "", "", "");

      CHECK(draft.kind == rt::LibraryMutationService::ListKind::Smart);
      CHECK(draft.parentId == kInvalidListId);
      CHECK(draft.listId == kInvalidListId);
      CHECK(draft.name.empty());
      CHECK(draft.description.empty());
      CHECK(draft.expression.empty());
    }
  }
} // namespace ao::uimodel::list::test
