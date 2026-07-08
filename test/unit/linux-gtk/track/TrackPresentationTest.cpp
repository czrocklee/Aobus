// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/app/GtkLayoutConfig.h"
#include "test/unit/TestUtils.h"
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("GtkLayoutConfig - persists track column layouts to gtk_layout.yaml", "[gtk][unit][track][presentation]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configDir = std::filesystem::path{tempDir.path()} / ".aobus";
    auto const configPath = configDir / "gtk_layout.yaml";

    auto state = uimodel::TrackColumnLayoutState{};
    auto prefState = uimodel::ListPresentationPreferenceState{};
    auto layout =
      std::vector{uimodel::TrackColumnState{.field = rt::TrackField::Title, .width = -1, .weight = 1.25},
                  uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 222, .weight = -1.0}};
    state.listLayouts.emplace(ListId{42}, layout);

    {
      auto config = GtkLayoutConfig{configDir};
      config.save(state, prefState);
    }

    REQUIRE(std::filesystem::exists(configPath));

    auto loaded = uimodel::TrackColumnLayoutState{};
    auto loadedPref = uimodel::ListPresentationPreferenceState{};
    auto config = GtkLayoutConfig{configDir};
    config.load(loaded, loadedPref);

    REQUIRE(loaded.listLayouts.contains(ListId{42}));
    auto const& loadedLayout = loaded.listLayouts.at(ListId{42});
    REQUIRE(loadedLayout.size() == 2);
    CHECK(loadedLayout[0].field == rt::TrackField::Title);
    CHECK(loadedLayout[0].width == -1);
    CHECK(loadedLayout[0].weight == 1.25);
    CHECK(loadedLayout[1].field == rt::TrackField::Duration);
    CHECK(loadedLayout[1].width == 222);
    CHECK(loadedLayout[1].weight == -1.0);
  }
} // namespace ao::gtk::test
