// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/SmartListEditorViewState.h>
#include <ao/uimodel/library/list/SmartListPreview.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("SmartListEditorViewState - hides unavailable preview source", "[uimodel][unit][list]")
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

  TEST_CASE("SmartListEditorViewState - shows full source for empty local expression", "[uimodel][unit][list]")
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

  TEST_CASE("SmartListEditorViewState - uses library wording for empty all-track source", "[uimodel][unit][list]")
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

  TEST_CASE("SmartListEditorViewState - exposes query errors and hides preview", "[uimodel][unit][list]")
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

  TEST_CASE("SmartListEditorViewState - keeps empty invalid filter preview visible", "[uimodel][unit][list]")
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
