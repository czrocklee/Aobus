// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/app/GtkLayoutConfig.h"
#include "app/linux-gtk/app/UIState.h"
#include "app/linux-gtk/track/TrackFieldUi.h"
#include "runtime/TrackField.h"
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
  }

  TEST_CASE("TrackPresentation - field UI definitions exist for all presentable fields", "[app][presentation]")
  {
    for (auto const& rtDef : rt::trackFieldDefinitions())
    {
      if (rtDef.presentable)
      {
        INFO("Field " << rtDef.id << " must have a UI definition");
        CHECK(trackFieldUiDefinition(rtDef.field) != nullptr);
      }
    }
  }

  TEST_CASE("TrackPresentation - default width for fields", "[app][presentation]")
  {
    CHECK(defaultWidthForField(rt::TrackField::Artist) == 150);
    CHECK(defaultWidthForField(rt::TrackField::Album) == 200);
    CHECK(defaultWidthForField(rt::TrackField::TrackNumber) == 72);
    CHECK(defaultWidthForField(rt::TrackField::Duration) == 84);
    CHECK(defaultWidthForField(rt::TrackField::Year) == 80);
    CHECK(defaultWidthForField(rt::TrackField::AlbumArtist) == 180);
  }

  TEST_CASE("TrackPresentation - field is expanding", "[app][presentation]")
  {
    CHECK(fieldIsExpanding(rt::TrackField::Tags));
    CHECK_FALSE(fieldIsExpanding(rt::TrackField::Title));
  }

  TEST_CASE("TrackPresentation - field visible by default", "[app][presentation]")
  {
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Title));
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Artist));
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Album));
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Duration));
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Tags));

    CHECK_FALSE(fieldIsVisibleByDefault(rt::TrackField::AlbumArtist));
    CHECK_FALSE(fieldIsVisibleByDefault(rt::TrackField::Genre));
    CHECK_FALSE(fieldIsVisibleByDefault(rt::TrackField::Year));
  }

  TEST_CASE("TrackPresentation - field column title", "[app][presentation]")
  {
    CHECK(fieldColumnTitle(rt::TrackField::Title) == "Title");
    CHECK(fieldColumnTitle(rt::TrackField::Artist) == "Artist");
    CHECK(fieldColumnTitle(rt::TrackField::Duration) == "Duration");
  }

  TEST_CASE("TrackPresentation - redundantFieldToColumn maps sort fields to TrackField", "[app][presentation]")
  {
    CHECK(redundantFieldToColumn(rt::TrackSortField::Artist) == rt::TrackField::Artist);
    CHECK(redundantFieldToColumn(rt::TrackSortField::Album) == rt::TrackField::Album);
    CHECK(redundantFieldToColumn(rt::TrackSortField::AlbumArtist) == rt::TrackField::AlbumArtist);
    CHECK(redundantFieldToColumn(rt::TrackSortField::Genre) == rt::TrackField::Genre);
    CHECK(redundantFieldToColumn(rt::TrackSortField::Composer) == rt::TrackField::Composer);
    CHECK(redundantFieldToColumn(rt::TrackSortField::Work) == rt::TrackField::Work);
    CHECK(redundantFieldToColumn(rt::TrackSortField::Year) == rt::TrackField::Year);

    CHECK_FALSE(redundantFieldToColumn(rt::TrackSortField::Title).has_value());
    CHECK_FALSE(redundantFieldToColumn(rt::TrackSortField::Duration).has_value());
    CHECK_FALSE(redundantFieldToColumn(rt::TrackSortField::DiscNumber).has_value());
    CHECK_FALSE(redundantFieldToColumn(rt::TrackSortField::TrackNumber).has_value());
  }

  TEST_CASE("GtkLayoutConfig persists column layouts to gtk_layout.yaml", "[app][presentation]")
  {
    auto const tempDir = ao::lmdb::test::TempDir{};
    auto const configDir = std::filesystem::path{tempDir.path()} / ".aobus";
    auto const configPath = configDir / "gtk_layout.yaml";

    auto state = ColumnLayoutState{};
    auto layout = std::vector<ColumnState>{
      {.field = rt::TrackField::Title, .width = 321},
      {.field = rt::TrackField::Artist, .width = 222},
    };
    state.listLayouts.emplace(ao::ListId{42}, layout);

    {
      auto config = GtkLayoutConfig{configDir};
      config.save(state);
    }

    REQUIRE(std::filesystem::exists(configPath));

    auto loaded = ColumnLayoutState{};
    auto config = GtkLayoutConfig{configDir};
    config.load(loaded);

    REQUIRE(loaded.listLayouts.contains(ao::ListId{42}));
    auto const& loadedLayout = loaded.listLayouts.at(ao::ListId{42});
    REQUIRE(loadedLayout.size() == 2);
    CHECK(loadedLayout[0].field == rt::TrackField::Title);
    CHECK(loadedLayout[0].width == 321);
    CHECK(loadedLayout[1].field == rt::TrackField::Artist);
    CHECK(loadedLayout[1].width == 222);
  }
} // namespace ao::gtk::test
