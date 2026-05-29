// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkLayoutConfig.h"

#include "app/UIState.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace ao::gtk::test
{
  using namespace ao::lmdb::test;

  TEST_CASE("GtkLayoutConfig - persistence", "[gtk][app][config]")
  {
    auto const tempDir = TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};

    SECTION("Load non-existent config returns default")
    {
      auto const config = GtkLayoutConfig{libraryPath};
      auto state = ColumnLayoutState{};
      state.listLayouts[ListId{1}] = {{rt::TrackField::Title, 100}};

      config.load(state);
      // Not found should not modify
      CHECK(state.listLayouts[ListId{1}][0].width == 100);
    }

    SECTION("Save and load layout state")
    {
      {
        auto config = GtkLayoutConfig{libraryPath};
        auto state = ColumnLayoutState{};
        state.listLayouts[rt::kAllTracksListId] = {{rt::TrackField::Artist, 250}};
        config.save(state);
      }

      {
        auto const config = GtkLayoutConfig{libraryPath};
        auto state = ColumnLayoutState{};
        config.load(state);
        REQUIRE(state.listLayouts.contains(rt::kAllTracksListId));
        CHECK(state.listLayouts[rt::kAllTracksListId][0].width == 250);
      }
    }
  }
} // namespace ao::gtk::test
