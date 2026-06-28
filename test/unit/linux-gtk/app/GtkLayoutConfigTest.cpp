// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkLayoutConfig.h"

#include "test/unit/TestUtils.h"
#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackColumnLayoutStore.h>
#include <ao/uimodel/track/TrackPresentationPreferenceStore.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace ao::gtk::test
{
  TEST_CASE("GtkLayoutConfig persists column layout preferences", "[gtk][unit][app][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};

    SECTION("Load non-existent config returns default")
    {
      auto const config = GtkLayoutConfig{libraryPath};
      auto newState = uimodel::track::TrackColumnLayoutState{};
      auto newPrefState = uimodel::track::ListPresentationPreferenceState{};
      config.load(newState, newPrefState);
      // Not found should not modify
      CHECK(newState.listLayouts.empty());
      CHECK(newPrefState.presentations.empty());
    }

    SECTION("Save and load layout state")
    {
      {
        auto config = GtkLayoutConfig{libraryPath};
        auto state = uimodel::track::TrackColumnLayoutState{};
        auto prefState = uimodel::track::ListPresentationPreferenceState{};
        state.listLayouts[ListId{10}] = {uimodel::track::ColumnState{.field = rt::TrackField::Artist, .width = 150}};
        state.listLayouts[ListId{20}] = {uimodel::track::ColumnState{.field = rt::TrackField::Album, .width = 200}};
        prefState.presentations[ListId{10}] = "albums";
        config.save(state, prefState);
      }

      {
        auto const config = GtkLayoutConfig{libraryPath};
        auto state = uimodel::track::TrackColumnLayoutState{};
        auto prefState = uimodel::track::ListPresentationPreferenceState{};
        config.load(state, prefState);
        REQUIRE(state.listLayouts.size() == 2);
        CHECK(state.listLayouts[ListId{10}][0].width == 150);
        REQUIRE(prefState.presentations.size() == 1);
        CHECK(prefState.presentations[ListId{10}] == "albums");
      }
    }
  }
} // namespace ao::gtk::test
