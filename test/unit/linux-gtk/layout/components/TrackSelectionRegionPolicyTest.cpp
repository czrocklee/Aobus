// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/track/TrackSelectionRegionPolicy.h"

#include <ao/rt/projection/ProjectionTypes.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::layout::track_selection_region::test
{
  TEST_CASE("TrackSelectionRegionPolicy shows regions for matching selection modes",
            "[gtk][unit][selection-region][policy]")
  {
    CHECK(presentationForSelection(rt::SelectionKind::None, "none", false).visible);
    CHECK_FALSE(presentationForSelection(rt::SelectionKind::Single, "none", false).visible);

    CHECK(presentationForSelection(rt::SelectionKind::Single, "single", false).visible);
    CHECK_FALSE(presentationForSelection(rt::SelectionKind::Multiple, "single", false).visible);

    CHECK(presentationForSelection(rt::SelectionKind::Multiple, "multiple", false).visible);
    CHECK_FALSE(presentationForSelection(rt::SelectionKind::None, "multiple", false).visible);

    CHECK(presentationForSelection(rt::SelectionKind::Single, "any", false).visible);
    CHECK(presentationForSelection(rt::SelectionKind::Multiple, "any", false).visible);
    CHECK_FALSE(presentationForSelection(rt::SelectionKind::None, "any", false).visible);

    CHECK_FALSE(presentationForSelection(rt::SelectionKind::Single, "unknown", false).visible);
  }

  TEST_CASE("TrackSelectionRegionPolicy keeps placeholders visible but disabled without selection",
            "[gtk][unit][selection-region][policy]")
  {
    auto const placeholder = presentationForSelection(rt::SelectionKind::None, "any", true);
    CHECK(placeholder.visible);
    CHECK_FALSE(placeholder.sensitive);

    auto const selected = presentationForSelection(rt::SelectionKind::Single, "any", true);
    CHECK(selected.visible);
    CHECK(selected.sensitive);

    auto const hiddenPlaceholderMode = presentationForSelection(rt::SelectionKind::None, "single", true);
    CHECK_FALSE(hiddenPlaceholderMode.visible);
    CHECK_FALSE(hiddenPlaceholderMode.sensitive);
  }
} // namespace ao::gtk::layout::track_selection_region::test
