// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackSection.h"

#include "test/unit/PresentationTextCatalogTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::tui::test
{
  TEST_CASE("TrackSection - display names fall back consistently", "[tui][unit][track-section]")
  {
    auto const& english = ao::test::englishPresentationTextCatalog();
    CHECK(trackSectionDisplayName(english, TrackSection{.primaryText = "Album A"}) == "Album A");
    CHECK(trackSectionDisplayName(english, TrackSection{}) == "Untitled Section");

    auto const german = ao::test::presentationTextCatalog("de-AT");
    CHECK(trackSectionDisplayName(german, TrackSection{}) == "Unbenannter Abschnitt");
  }
} // namespace ao::tui::test
