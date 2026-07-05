// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkLayoutConfig.h"

#include "test/unit/TestUtils.h"
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace ao::gtk::test
{
  TEST_CASE("GtkLayoutConfig persists column layout preferences", "[gtk][unit][app][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};

    SECTION("Load non-existent config returns default")
    {
      auto const config = GtkLayoutConfig{libraryPath};
      auto newState = uimodel::TrackColumnLayoutState{};
      auto newPrefState = uimodel::ListPresentationPreferenceState{};
      config.load(newState, newPrefState);
      // Not found should not modify
      CHECK(newState.listLayouts.empty());
      CHECK(newPrefState.presentations.empty());
    }

    SECTION("Save and load layout state")
    {
      {
        auto config = GtkLayoutConfig{libraryPath};
        auto state = uimodel::TrackColumnLayoutState{};
        auto prefState = uimodel::ListPresentationPreferenceState{};
        state.listLayouts[ListId{10}] = {
          uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = -1, .weight = 1.75}};
        state.listLayouts[ListId{20}] = {
          uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 200, .weight = -1.0}};
        prefState.presentations[ListId{10}] = "albums";
        config.save(state, prefState);
      }

      {
        auto const config = GtkLayoutConfig{libraryPath};
        auto state = uimodel::TrackColumnLayoutState{};
        auto prefState = uimodel::ListPresentationPreferenceState{};
        config.load(state, prefState);
        REQUIRE(state.listLayouts.size() == 2);
        CHECK(state.listLayouts[ListId{10}][0].field == rt::TrackField::Artist);
        CHECK(state.listLayouts[ListId{10}][0].weight == 1.75);
        CHECK(state.listLayouts[ListId{20}][0].field == rt::TrackField::Duration);
        CHECK(state.listLayouts[ListId{20}][0].width == 200);
        REQUIRE(prefState.presentations.size() == 1);
        CHECK(prefState.presentations[ListId{10}] == "albums");
      }
    }

    SECTION("Load old column layout entries without weight")
    {
      std::filesystem::create_directories(libraryPath);
      auto output = std::ofstream{libraryPath / "gtk_layout.yaml"};
      output << "trackView.columnLayouts:\n"
                "  listLayouts:\n"
                "    42:\n"
                "      - field: 0\n"
                "        width: 321\n";
      output.close();

      auto const config = GtkLayoutConfig{libraryPath};
      auto state = uimodel::TrackColumnLayoutState{};
      auto prefState = uimodel::ListPresentationPreferenceState{};
      config.load(state, prefState);

      REQUIRE(state.listLayouts.contains(ListId{42}));
      REQUIRE(state.listLayouts[ListId{42}].size() == 1);
      CHECK(state.listLayouts[ListId{42}][0].field == rt::TrackField::Title);
      CHECK(state.listLayouts[ListId{42}][0].width == 321);
      CHECK(state.listLayouts[ListId{42}][0].weight == -1.0);
    }
  }
} // namespace ao::gtk::test
