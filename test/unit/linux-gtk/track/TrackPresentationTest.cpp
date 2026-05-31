// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/app/GtkLayoutConfig.h"
#include "app/linux-gtk/track/TrackFieldUi.h"
#include "test/unit/TestUtils.h"
#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
  }

  TEST_CASE("TrackPresentation - field UI definitions exist for all presentable fields", "[app][unit][presentation]")
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

  TEST_CASE("TrackPresentation - default width for fields", "[app][unit][presentation]")
  {
    CHECK(defaultWidthForField(rt::TrackField::Artist) == 150);
    CHECK(defaultWidthForField(rt::TrackField::Album) == 200);
    CHECK(defaultWidthForField(rt::TrackField::TrackNumber) == 72);
    CHECK(defaultWidthForField(rt::TrackField::Duration) == 84);
    CHECK(defaultWidthForField(rt::TrackField::Year) == 80);
    CHECK(defaultWidthForField(rt::TrackField::AlbumArtist) == 180);
  }

  TEST_CASE("TrackPresentation - field visible by default", "[app][unit][presentation]")
  {
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Title));
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Artist));
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Album));
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Year));
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Duration));
    CHECK(fieldIsVisibleByDefault(rt::TrackField::Tags));
    CHECK(fieldIsVisibleByDefault(rt::TrackField::DisplayTrackNumber));

    CHECK_FALSE(fieldIsVisibleByDefault(rt::TrackField::AlbumArtist));
    CHECK_FALSE(fieldIsVisibleByDefault(rt::TrackField::Genre));
    CHECK_FALSE(fieldIsVisibleByDefault(rt::TrackField::TrackNumber));
  }

  TEST_CASE("TrackPresentation - field column title", "[app][unit][presentation]")
  {
    CHECK(fieldColumnTitle(rt::TrackField::Title) == "Title");
    CHECK(fieldColumnTitle(rt::TrackField::Artist) == "Artist");
    CHECK(fieldColumnTitle(rt::TrackField::Duration) == "Duration");
  }

  TEST_CASE("TrackPresentation - redundantFieldToColumn maps sort fields to TrackField", "[app][unit][presentation]")
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

  TEST_CASE("GtkLayoutConfig persists column layouts to gtk_layout.yaml", "[app][unit][presentation]")
  {
    auto const tempDir = ao::test::TempDir{}; // NOLINT(aobus-readability-redundant-namespace-qualification)
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
