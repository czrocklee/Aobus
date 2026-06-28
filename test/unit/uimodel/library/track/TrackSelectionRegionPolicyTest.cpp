// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/library/track/TrackSelectionRegionPolicy.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("presentationForTrackSelectionRegion shows regions for matching selection modes",
            "[uimodel][unit][library][track]")
  {
    CHECK(presentationForTrackSelectionRegion(rt::SelectionKind::None, "none", false).visible);
    CHECK_FALSE(presentationForTrackSelectionRegion(rt::SelectionKind::Single, "none", false).visible);

    CHECK(presentationForTrackSelectionRegion(rt::SelectionKind::Single, "single", false).visible);
    CHECK_FALSE(presentationForTrackSelectionRegion(rt::SelectionKind::Multiple, "single", false).visible);

    CHECK(presentationForTrackSelectionRegion(rt::SelectionKind::Multiple, "multiple", false).visible);
    CHECK_FALSE(presentationForTrackSelectionRegion(rt::SelectionKind::None, "multiple", false).visible);

    CHECK(presentationForTrackSelectionRegion(rt::SelectionKind::Single, "any", false).visible);
    CHECK(presentationForTrackSelectionRegion(rt::SelectionKind::Multiple, "any", false).visible);
    CHECK_FALSE(presentationForTrackSelectionRegion(rt::SelectionKind::None, "any", false).visible);

    CHECK_FALSE(presentationForTrackSelectionRegion(rt::SelectionKind::Single, "unknown", false).visible);
  }

  TEST_CASE("presentationForTrackSelectionRegion keeps placeholders visible but disabled without selection",
            "[uimodel][unit][library][track]")
  {
    auto const placeholder = presentationForTrackSelectionRegion(rt::SelectionKind::None, "any", true);
    CHECK(placeholder.visible);
    CHECK_FALSE(placeholder.sensitive);

    auto const selected = presentationForTrackSelectionRegion(rt::SelectionKind::Single, "any", true);
    CHECK(selected.visible);
    CHECK(selected.sensitive);

    auto const hiddenPlaceholderMode = presentationForTrackSelectionRegion(rt::SelectionKind::None, "single", true);
    CHECK_FALSE(hiddenPlaceholderMode.visible);
    CHECK_FALSE(hiddenPlaceholderMode.sensitive);
  }
} // namespace ao::uimodel::test
