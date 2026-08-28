// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/app/GtkLayoutStateStore.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("GtkLayoutStateStore - persists track column layouts to gtk_layout.yaml",
            "[gtk][unit][track][presentation]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const& configDir = tempDir.path();
    auto const configPath = configDir / "gtk_layout.yaml";

    auto state = uimodel::TrackColumnLayouts::Snapshot{};
    auto prefState = uimodel::ListPresentations::Snapshot{};
    auto layout =
      std::vector{uimodel::TrackColumnState{.field = rt::TrackField::Title, .width = -1, .weight = 1.25},
                  uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 222, .weight = -1.0}};
    state.emplace(ListId{42}, layout);

    {
      auto store = GtkLayoutStateStore{configDir};
      store.save(state, prefState);
    }

    REQUIRE(std::filesystem::exists(configPath));

    auto loaded = uimodel::TrackColumnLayouts::Snapshot{};
    auto loadedPref = uimodel::ListPresentations::Snapshot{};
    auto store = GtkLayoutStateStore{configDir};
    store.load(loaded, loadedPref);

    REQUIRE(loaded.contains(ListId{42}));
    auto const& loadedLayout = loaded.at(ListId{42});
    REQUIRE(loadedLayout.size() == 2);
    CHECK(loadedLayout[0].field == rt::TrackField::Title);
    CHECK(loadedLayout[0].width == -1);
    CHECK(loadedLayout[0].weight == 1.25);
    CHECK(loadedLayout[1].field == rt::TrackField::Duration);
    CHECK(loadedLayout[1].width == 222);
    CHECK(loadedLayout[1].weight == -1.0);
  }
} // namespace ao::gtk::test
