// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryWriter.h>
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
    auto const small = formatSmartListPreviewStatusText(SmartListPreviewStatus::Valid, 3, true, false);
    auto const large = formatSmartListPreviewStatusText(SmartListPreviewStatus::Valid, 14, true, false);

    CHECK(formatSmartListPreviewStatusText(SmartListPreviewStatus::Valid, 0, true, false) == "No matches");
    CHECK(small == "Showing all 3 matches");
    CHECK(large == "Showing 10 of 14 matches");
  }

  TEST_CASE("SmartListEditorModel - formats an unfiltered source with track-count grammar", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListPreviewStatusText(SmartListPreviewStatus::Valid, 1, true, true) == "Showing all 1 track");
    CHECK(formatSmartListPreviewStatusText(SmartListPreviewStatus::Valid, 1, false, true) ==
          "Showing all 1 track from source");
    CHECK(formatSmartListPreviewStatusText(SmartListPreviewStatus::Valid, 4, false, true) ==
          "Showing all 4 tracks from source");
  }

  TEST_CASE("SmartListEditorModel - formats unavailable source status text", "[uimodel][unit][list]")
  {
    CHECK(deriveSmartListPreviewStatus(true, false) == SmartListPreviewStatus::PreviewSourceUnavailable);
    CHECK(formatSmartListPreviewStatusText(SmartListPreviewStatus::PreviewSourceUnavailable, 0, false, false) ==
          "No tracks in source");
  }

  TEST_CASE("SmartListEditorModel - falls back for unknown status", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListPreviewStatusText(static_cast<SmartListPreviewStatus>(250), 2, true, false).empty());
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

  TEST_CASE("SmartListPreviewStatus - keeps stable enum values", "[uimodel][regression][list]")
  {
    CHECK(static_cast<int>(SmartListPreviewStatus::PreviewSourceUnavailable) == 0);
    CHECK(static_cast<int>(SmartListPreviewStatus::Valid) == 1);
    CHECK(static_cast<int>(SmartListPreviewStatus::InvalidExpression) == 2);
  }

  TEST_CASE("SmartListEditorModel - accepts named valid draft", "[uimodel][unit][list]")
  {
    CHECK(canSubmitSmartListDraft("My Smart List", SmartListPreviewStatus::Valid) == true);
  }

  TEST_CASE("SmartListEditorModel - rejects empty name", "[uimodel][unit][list]")
  {
    CHECK(canSubmitSmartListDraft("", SmartListPreviewStatus::Valid) == false);
  }

  TEST_CASE("SmartListEditorModel - rejects invalid expression", "[uimodel][unit][list]")
  {
    CHECK(canSubmitSmartListDraft("My List", SmartListPreviewStatus::InvalidExpression) == false);
  }

  TEST_CASE("SmartListEditorModel - rejects empty invalid draft", "[uimodel][unit][list]")
  {
    CHECK(canSubmitSmartListDraft("", SmartListPreviewStatus::InvalidExpression) == false);
  }

  TEST_CASE("SmartListEditorModel - accepts named draft with unavailable preview source", "[uimodel][unit][list]")
  {
    CHECK(canSubmitSmartListDraft("My List", SmartListPreviewStatus::PreviewSourceUnavailable) == true);
  }

  TEST_CASE("SmartListEditorModel - preserves parent edit id and filter fields", "[uimodel][unit][list]")
  {
    auto const parentListId = ListId{10};
    auto const editListId = ListId{42};
    auto const name = std::string{"My Smart List"};
    auto const description = std::string{"A description"};
    auto const expression = std::string{"$artist = 'Queen'"};

    auto const draft = makeSmartListDraft(parentListId, editListId, name, description, expression);

    CHECK(draft.kind == rt::LibraryWriter::ListKind::Smart);
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

    CHECK(draft.kind == rt::LibraryWriter::ListKind::Smart);
    CHECK(draft.parentId == parentListId);
    CHECK(draft.listId == kInvalidListId);
    CHECK(draft.name == name);
    CHECK(draft.description == description);
    CHECK(draft.expression == expression);
  }

  TEST_CASE("SmartListEditorModel - preserves empty text fields", "[uimodel][unit][list]")
  {
    auto const draft = makeSmartListDraft(kInvalidListId, kInvalidListId, "", "", "");

    CHECK(draft.kind == rt::LibraryWriter::ListKind::Smart);
    CHECK(draft.parentId == kInvalidListId);
    CHECK(draft.listId == kInvalidListId);
    CHECK(draft.name.empty());
    CHECK(draft.description.empty());
    CHECK(draft.expression.empty());
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

    CHECK(state.status == SmartListPreviewStatus::PreviewSourceUnavailable);
    CHECK(state.name == "Library Picks");
    CHECK(state.localExpression == "$artist = 'Queen'");
    CHECK(state.matchCount == 0);
    CHECK(state.isAllTracks == false);
    CHECK(state.previewStatusText.empty());
    CHECK(state.errorText.empty());
    CHECK(state.expressionValid == false);
    CHECK(state.queryInvalid == false);
    CHECK(state.previewVisible == false);
    CHECK(state.errorVisible == false);
    CHECK(state.canSubmit == false);
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

    CHECK(state.status == SmartListPreviewStatus::Valid);
    CHECK(state.name == "Source Tracks");
    CHECK(state.localExpression.empty());
    CHECK(state.matchCount == 4);
    CHECK(state.isAllTracks == false);
    CHECK(state.previewStatusText == "Showing all 4 tracks from source");
    CHECK(state.expressionValid == true);
    CHECK(state.queryInvalid == false);
    CHECK(state.previewVisible == true);
    CHECK(state.errorVisible == false);
    CHECK(state.errorText.empty());
    CHECK(state.canSubmit == true);
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

    CHECK(state.status == SmartListPreviewStatus::InvalidExpression);
    CHECK(state.name == "Broken Filter");
    CHECK(state.localExpression == "$artist =");
    CHECK(state.matchCount == 0);
    CHECK(state.isAllTracks == true);
    CHECK(state.previewStatusText == "Invalid filter");
    CHECK(state.queryInvalid == true);
    CHECK(state.errorVisible == true);
    CHECK(state.previewVisible == false);
    CHECK(state.expressionValid == false);
    CHECK(state.errorText == "Filter error: expected value");
    CHECK(state.canSubmit == false);
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
    CHECK(state.canSubmit == true);
  }
} // namespace ao::uimodel::test
