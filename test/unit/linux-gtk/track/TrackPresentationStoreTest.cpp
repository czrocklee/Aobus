// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationStore.h"

#include "app/UIState.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TrackPresentationStore - preset and layout management", "[gtk][track][presentation]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& workspace = fixture.runtime().workspace();

    auto store = TrackPresentationStore{workspace};

    SECTION("initial presets")
    {
      CHECK_FALSE(store.builtinPresets().empty());
    }

    SECTION("active presentation management")
    {
      store.setActivePresentationId("default");
      CHECK(store.activePresentationId() == "default");
    }

    SECTION("list layout persistence")
    {
      auto const listId = ListId{1};
      auto const layout = std::vector<ColumnState>{{rt::TrackField::Title, 200}};

      store.updateLayout(listId, layout);

      auto const& retrieved = store.layoutForList(listId);
      REQUIRE(retrieved.size() == 1);
      CHECK(retrieved[0].field == rt::TrackField::Title);
      CHECK(retrieved[0].width == 200);
    }

    SECTION("signal propagation on change")
    {
      auto changed = false;
      store.signalChanged().connect([&](auto, auto) { changed = true; });

      store.setActivePresentationId("modern");
      CHECK(changed == true);
    }
  }
} // namespace ao::gtk::test
