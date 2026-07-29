// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/uimodel/library/list/SmartListEditorModel.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::uimodel::test
{
  TEST_CASE("SmartListEditorModel - formats empty expression as none", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListExpressionDisplayText("") == "(none)");
  }

  TEST_CASE("SmartListEditorModel - preserves non-empty display text", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListExpressionDisplayText("$genre = 'Jazz'") == "$genre = 'Jazz'");
  }

  TEST_CASE("SmartListEditorModel - returns local expression without parent", "[uimodel][unit][list]")
  {
    CHECK(combineSmartListEffectiveExpression("", "$artist = 'Queen'") == "$artist = 'Queen'");
  }

  TEST_CASE("SmartListEditorModel - combines parent and local expressions", "[uimodel][unit][list]")
  {
    auto const effective = combineSmartListEffectiveExpression("$year > 1970", "$artist = 'Queen'");
    CHECK(effective == "($year > 1970) and ($artist = 'Queen')");
  }

  TEST_CASE("SmartListEditorModel - returns parent expression without local filter", "[uimodel][unit][list]")
  {
    CHECK(combineSmartListEffectiveExpression("$year > 1970", "") == "$year > 1970");
  }

  TEST_CASE("SmartListEditorModel - formats status text", "[uimodel][unit][list]")
  {
    auto const small = formatSmartListPreviewStatusText(true, 3, true, false);
    auto const large = formatSmartListPreviewStatusText(true, 14, true, false);

    CHECK(formatSmartListPreviewStatusText(true, 0, true, false) == "No matches");
    CHECK(small == "Showing all 3 matches");
    CHECK(large == "Showing 10 of 14 matches");
  }

  TEST_CASE("SmartListEditorModel - formats an unfiltered source with track-count grammar", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListPreviewStatusText(true, 1, true, true) == "Showing all 1 track");
    CHECK(formatSmartListPreviewStatusText(true, 1, false, true) == "Showing all 1 track from source");
    CHECK(formatSmartListPreviewStatusText(true, 4, false, true) == "Showing all 4 tracks from source");
  }

  TEST_CASE("SmartListEditorModel - formats an invalid expression", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListPreviewStatusText(false, 0, false, false) == "Invalid filter");
  }

  TEST_CASE("SmartListEditorModel - formats track labels", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListPreviewTrackLabel("Blue in Green", "Miles Davis", "Kind of Blue") ==
          "Blue in Green - Miles Davis (Kind of Blue)");
    CHECK(formatSmartListPreviewTrackLabel("Blue in Green", "", "Kind of Blue") == "Blue in Green (Kind of Blue)");
    CHECK(formatSmartListPreviewTrackLabel("", "Miles Davis", "Kind of Blue") == "Miles Davis (Kind of Blue)");
    CHECK(formatSmartListPreviewTrackLabel("", "Miles Davis", "") == "Miles Davis");
    CHECK(formatSmartListPreviewTrackLabel("", "", "Kind of Blue") == "(untitled)");
    CHECK(formatSmartListPreviewTrackLabel("", "", "") == "(untitled)");
  }

  TEST_CASE("SmartListEditorModel - preserves parent edit id and filter fields", "[uimodel][unit][list]")
  {
    auto const parentListId = ListId{10};
    auto const editListId = ListId{42};
    auto const name = std::string{"My Smart List"};
    auto const description = std::string{"A description"};
    auto const expression = std::string{"$artist = 'Queen'"};

    auto const draft = makeSmartListDraft(parentListId, editListId, name, description, expression);

    CHECK(draft.parentId == parentListId);
    CHECK(draft.listId == editListId);
    CHECK(draft.name == name);
    CHECK(draft.description == description);
    CHECK(draft.expression == expression);
  }

  TEST_CASE("SmartListEditorModel - preserves create sentinel id", "[uimodel][unit][list]")
  {
    auto const parentListId = ListId{10};
    auto const name = std::string{"My Smart List"};
    auto const description = std::string{"A description"};
    auto const expression = std::string{"$artist = 'Queen'"};

    auto const draft = makeSmartListDraft(parentListId, kInvalidListId, name, description, expression);

    CHECK(draft.parentId == parentListId);
    CHECK(draft.listId == kInvalidListId);
    CHECK(draft.name == name);
    CHECK(draft.description == description);
    CHECK(draft.expression == expression);
  }

  TEST_CASE("SmartListEditorModel - preserves empty text fields", "[uimodel][unit][list]")
  {
    auto const draft = makeSmartListDraft(kInvalidListId, kInvalidListId, "", "", "");

    CHECK(draft.parentId == kInvalidListId);
    CHECK(draft.listId == kInvalidListId);
    CHECK(draft.name.empty());
    CHECK(draft.description.empty());
    CHECK(draft.expression.empty());
  }

  TEST_CASE("SmartListEditorModel - explains direct and computed membership while editing",
            "[uimodel][unit][list][writable-tag]")
  {
    auto const direct = makeSmartListEditorViewState(SmartListPreviewState{
      .name = "Road Trip",
      .localExpression = R"(#"road-trip")",
      .hasPreviewSource = true,
      .hasError = false,
      .errorMessage = "",
      .matchCount = 0,
      .isAllTracks = true,
    });
    CHECK(direct.hasDirectMembershipEditing);
    CHECK(direct.membershipEditingText == R"(Direct membership editing via #"road-trip")");

    auto const computed = makeSmartListEditorViewState(SmartListPreviewState{
      .name = "Recent Road Trip",
      .localExpression = R"(#"road-trip" and $year >= 2020)",
      .hasPreviewSource = true,
      .hasError = false,
      .errorMessage = "",
      .matchCount = 0,
      .isAllTracks = true,
    });
    CHECK_FALSE(computed.hasDirectMembershipEditing);
    CHECK(computed.membershipEditingText == "Computed membership — edit tags or the expression");
  }

  TEST_CASE("SmartListEditorModel - hides unavailable preview source", "[uimodel][unit][list]")
  {
    auto const state = makeSmartListEditorViewState(SmartListPreviewState{
      .name = "Library Picks",
      .localExpression = "$artist = 'Queen'",
      .hasPreviewSource = false,
      .hasError = false,
      .errorMessage = "",
      .matchCount = 0,
      .isAllTracks = false,
    });

    CHECK(state.name == "Library Picks");
    CHECK(state.localExpression == "$artist = 'Queen'");
    CHECK(state.matchCount == 0);
    CHECK(state.isAllTracks == false);
    CHECK(state.previewStatusText.empty());
    CHECK(state.errorText.empty());
    CHECK(state.expressionValid == false);
    CHECK(state.queryInvalid == false);
    CHECK(state.canSubmit == false);
    CHECK(state.previewVisible == false);
    CHECK(state.errorVisible == false);
  }

  TEST_CASE("SmartListEditorModel - shows full source for empty local expression", "[uimodel][unit][list]")
  {
    auto const state = makeSmartListEditorViewState(SmartListPreviewState{
      .name = "Source Tracks",
      .localExpression = "",
      .hasPreviewSource = true,
      .hasError = false,
      .errorMessage = "",
      .matchCount = 4,
      .isAllTracks = false,
    });

    CHECK(state.name == "Source Tracks");
    CHECK(state.localExpression.empty());
    CHECK(state.matchCount == 4);
    CHECK(state.canSubmit);
    CHECK(state.isAllTracks == false);
    CHECK(state.previewStatusText == "Showing all 4 tracks from source");
    CHECK(state.expressionValid == true);
    CHECK(state.queryInvalid == false);
    CHECK(state.previewVisible == true);
    CHECK(state.errorVisible == false);
    CHECK(state.errorText.empty());

    auto const unnamed = makeSmartListEditorViewState(SmartListPreviewState{
      .name = "",
      .localExpression = "",
      .hasPreviewSource = true,
      .hasError = false,
      .errorMessage = "",
      .matchCount = 4,
      .isAllTracks = false,
    });
    CHECK_FALSE(unnamed.canSubmit);
  }

  TEST_CASE("SmartListEditorModel - uses library wording for empty all-track source", "[uimodel][unit][list]")
  {
    auto const state = makeSmartListEditorViewState(SmartListPreviewState{
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

  TEST_CASE("SmartListEditorModel - exposes query errors and hides preview", "[uimodel][unit][list]")
  {
    auto const state = makeSmartListEditorViewState(SmartListPreviewState{
      .name = "Broken Filter",
      .localExpression = "$artist =",
      .hasPreviewSource = true,
      .hasError = true,
      .errorMessage = "expected value",
      .matchCount = 0,
      .isAllTracks = true,
    });

    CHECK(state.name == "Broken Filter");
    CHECK(state.localExpression == "$artist =");
    CHECK(state.matchCount == 0);
    CHECK(state.isAllTracks == true);
    CHECK(state.previewStatusText == "Invalid filter");
    CHECK(state.queryInvalid == true);
    CHECK(state.errorVisible == true);
    CHECK(state.previewVisible == false);
    CHECK(state.expressionValid == false);
    CHECK_FALSE(state.canSubmit);
    CHECK(state.errorText == "Filter error: expected value");
  }

  TEST_CASE("SmartListEditorModel - keeps empty invalid filter preview visible", "[uimodel][unit][list]")
  {
    auto const state = makeSmartListEditorViewState(SmartListPreviewState{
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
    CHECK(state.errorText.empty());
    CHECK(state.previewStatusText == "Showing all 5 tracks");
  }
} // namespace ao::uimodel::test
