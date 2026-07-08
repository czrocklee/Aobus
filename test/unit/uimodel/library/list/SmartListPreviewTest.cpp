// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/SmartListPreview.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("SmartListPreview - formats status text", "[uimodel][unit][list]")
  {
    auto const small = formatSmartListPreviewStatusText(SmartListPreviewStatus::Valid, 3, true, false);
    auto const large = formatSmartListPreviewStatusText(SmartListPreviewStatus::Valid, 14, true, false);

    CHECK(formatSmartListPreviewStatusText(SmartListPreviewStatus::Valid, 0, true, false) == "No matches");
    CHECK(small == "Showing all 3 matches");
    CHECK(large == "Showing 10 of 14 matches");
  }

  TEST_CASE("SmartListPreview - formats unavailable source status text", "[uimodel][unit][list]")
  {
    CHECK(deriveSmartListPreviewStatus(true, false) == SmartListPreviewStatus::PreviewSourceUnavailable);
    CHECK(formatSmartListPreviewStatusText(SmartListPreviewStatus::PreviewSourceUnavailable, 0, false, false) ==
          "No tracks in source");
  }

  TEST_CASE("SmartListPreview - falls back for unknown status", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListPreviewStatusText(static_cast<SmartListPreviewStatus>(250), 2, true, false).empty());
  }

  TEST_CASE("SmartListPreview - formats track labels", "[uimodel][unit][list]")
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
} // namespace ao::uimodel::test
