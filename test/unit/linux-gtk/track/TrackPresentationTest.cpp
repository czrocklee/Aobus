// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/app/GtkLayoutConfig.h"
#include "test/unit/TestUtils.h"
#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackColumnLayoutStore.h>
#include <ao/uimodel/track/TrackPresentationPreferenceStore.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("GtkLayoutConfig persists track column layouts to gtk_layout.yaml", "[gtk][unit][track][presentation]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configDir = std::filesystem::path{tempDir.path()} / ".aobus";
    auto const configPath = configDir / "gtk_layout.yaml";

    auto state = uimodel::track::TrackColumnLayoutState{};
    auto prefState = uimodel::track::ListPresentationPreferenceState{};
    auto layout = std::vector{uimodel::track::ColumnState{.field = rt::TrackField::Title, .width = 321},
                              uimodel::track::ColumnState{.field = rt::TrackField::Artist, .width = 222}};
    state.listLayouts.emplace(ListId{42}, layout);

    {
      auto config = GtkLayoutConfig{configDir};
      config.save(state, prefState);
    }

    REQUIRE(std::filesystem::exists(configPath));

    auto loaded = uimodel::track::TrackColumnLayoutState{};
    auto loadedPref = uimodel::track::ListPresentationPreferenceState{};
    auto config = GtkLayoutConfig{configDir};
    config.load(loaded, loadedPref);

    REQUIRE(loaded.listLayouts.contains(ListId{42}));
    auto const& loadedLayout = loaded.listLayouts.at(ListId{42});
    REQUIRE(loadedLayout.size() == 2);
    CHECK(loadedLayout[0].field == rt::TrackField::Title);
    CHECK(loadedLayout[0].width == 321);
    CHECK(loadedLayout[1].field == rt::TrackField::Artist);
    CHECK(loadedLayout[1].width == 222);
  }
} // namespace ao::gtk::test
