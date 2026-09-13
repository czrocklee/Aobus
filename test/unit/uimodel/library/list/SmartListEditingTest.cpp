// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/SmartListEditing.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/CoreIds.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::uimodel::test
{
  TEST_CASE("SmartListEditorModel - formats empty expression as none", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListExpressionDisplayText(ao::test::englishMessageCatalog(), "") == "(none)");
  }

  TEST_CASE("SmartListEditorModel - preserves non-empty display text", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListExpressionDisplayText(ao::test::englishMessageCatalog(), "$genre = 'Jazz'") ==
          "$genre = 'Jazz'");
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

  TEST_CASE("SmartListEditorModel - distinguishes complete and partial filtered results", "[uimodel][regression][list]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();

    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 0, true, false) == "No matches");
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 1, true, false) == "Showing all matches: 1");
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 2, true, false) == "Showing all matches: 2");
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 10, true, false) == "Showing all matches: 10");
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 11, true, false) == "Showing 10 of 11 matches");
  }

  TEST_CASE("SmartListEditorModel - unfiltered previews describe the complete source", "[uimodel][regression][list]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 1, true, true) == "Showing all tracks: 1");
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 2, true, true) == "Showing all tracks: 2");
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 10, true, true) == "Showing all tracks: 10");
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 11, true, true) == "Showing all tracks: 11");

    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 1, false, true) == "Showing all tracks from source: 1");
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 2, false, true) == "Showing all tracks from source: 2");
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 10, false, true) == "Showing all tracks from source: 10");
    CHECK(formatSmartListPreviewStatusText(textCatalog, true, 11, false, true) == "Showing all tracks from source: 11");
  }

  TEST_CASE("SmartListEditorModel - formats an invalid expression", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListPreviewStatusText(ao::test::englishMessageCatalog(), false, 0, false, false) ==
          "Invalid filter");
  }

  TEST_CASE("SmartListEditorModel - formats track labels", "[uimodel][unit][list]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    CHECK(formatSmartListPreviewTrackLabel(textCatalog, "Blue in Green", "Miles Davis", "Kind of Blue") ==
          "Blue in Green - Miles Davis (Kind of Blue)");
    CHECK(formatSmartListPreviewTrackLabel(textCatalog, "Blue in Green", "", "Kind of Blue") ==
          "Blue in Green (Kind of Blue)");
    CHECK(formatSmartListPreviewTrackLabel(textCatalog, "", "Miles Davis", "Kind of Blue") ==
          "Miles Davis (Kind of Blue)");
    CHECK(formatSmartListPreviewTrackLabel(textCatalog, "", "Miles Davis", "") == "Miles Davis");
    CHECK(formatSmartListPreviewTrackLabel(textCatalog, "", "", "Kind of Blue") == "(untitled)");
    CHECK(formatSmartListPreviewTrackLabel(textCatalog, "", "", "") == "(untitled)");
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
    auto const direct = makeSmartListEditorViewState(ao::test::englishMessageCatalog(),
                                                     SmartListPreviewState{
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

    auto const computed = makeSmartListEditorViewState(ao::test::englishMessageCatalog(),
                                                       SmartListPreviewState{
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
    auto const state = makeSmartListEditorViewState(ao::test::englishMessageCatalog(),
                                                    SmartListPreviewState{
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
    auto const state = makeSmartListEditorViewState(ao::test::englishMessageCatalog(),
                                                    SmartListPreviewState{
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
    CHECK(state.previewStatusText == "Showing all tracks from source: 4");
    CHECK(state.expressionValid == true);
    CHECK(state.queryInvalid == false);
    CHECK(state.previewVisible == true);
    CHECK(state.errorVisible == false);
    CHECK(state.errorText.empty());

    auto const unnamed = makeSmartListEditorViewState(ao::test::englishMessageCatalog(),
                                                      SmartListPreviewState{
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
    auto const state = makeSmartListEditorViewState(ao::test::englishMessageCatalog(),
                                                    SmartListPreviewState{
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
    auto const state = makeSmartListEditorViewState(ao::test::englishMessageCatalog(),
                                                    SmartListPreviewState{
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
    auto const state = makeSmartListEditorViewState(ao::test::englishMessageCatalog(),
                                                    SmartListPreviewState{
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
    CHECK(state.previewStatusText == "Showing all tracks: 5");
  }
} // namespace ao::uimodel::test
