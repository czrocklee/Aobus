// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <gtkmm.h>

#include <algorithm>

#include <app/linux-gtk/track/TrackColumnController.h>
#include <app/linux-gtk/track/TrackPresentation.h>

namespace ao::gtk::test
{
  TEST_CASE("TrackPresentation - column definitions", "[app][presentation]")
  {
    auto const definitions = trackColumnDefinitions();
    REQUIRE(definitions.size() == 12);
  }

  TEST_CASE("TrackPresentation - default column layout", "[app][presentation]")
  {
    auto const layout = defaultTrackColumnLayout();
    REQUIRE(layout.columns.size() == trackColumnDefinitions().size());

    auto const duration = std::ranges::find(layout.columns, TrackColumn::Duration, &TrackColumnState::column);
    REQUIRE(duration != layout.columns.end());
    CHECK(duration->visible == true);
    CHECK(duration->width == 84);
  }

  TEST_CASE("TrackPresentation - normalize column layout fills missing and conceals them", "[app][presentation]")
  {
    auto layout = TrackColumnLayout{
      .columns = {{.column = TrackColumn::Title, .visible = true, .width = 300}},
    };
    auto const normalized = normalizeTrackColumnLayout(layout);

    REQUIRE(normalized.columns.size() == trackColumnDefinitions().size());

    // Explicitly provided column should keep its state
    auto const title = std::ranges::find(normalized.columns, TrackColumn::Title, &TrackColumnState::column);
    REQUIRE(title != normalized.columns.end());
    CHECK(title->visible == true);
    CHECK(title->width == 300);

    // Missing column (like Artist) should be added but set to HIDDEN
    auto const artist = std::ranges::find(normalized.columns, TrackColumn::Artist, &TrackColumnState::column);
    REQUIRE(artist != normalized.columns.end());
    CHECK(artist->visible == false);
  }

  TEST_CASE("TrackPresentation - layout for spec respects visible fields", "[app][presentation]")
  {
    using namespace ao::rt;

    auto spec = TrackPresentationSpec{
      .id = "test-album",
      .groupBy = TrackGroupKey::Album,
      .visibleFields = {TrackPresentationField::TrackNumber, TrackPresentationField::Title},
    };

    auto const layout = trackColumnLayoutForPresentation(spec);

    REQUIRE(layout.columns.size() == trackColumnDefinitions().size());

    // Requested fields should be visible
    auto const title = std::ranges::find(layout.columns, TrackColumn::Title, &TrackColumnState::column);
    CHECK(title->visible == true);

    auto const trackNum = std::ranges::find(layout.columns, TrackColumn::TrackNumber, &TrackColumnState::column);
    CHECK(trackNum->visible == true);

    // Unrequested fields (like Album or Artist) should be hidden
    auto const album = std::ranges::find(layout.columns, TrackColumn::Album, &TrackColumnState::column);
    CHECK(album->visible == false);

    auto const artist = std::ranges::find(layout.columns, TrackColumn::Artist, &TrackColumnState::column);
    CHECK(artist->visible == false);
  }

  TEST_CASE("TrackPresentation - expansion columns preserve a visible default expander", "[app][presentation]")
  {
    auto layout = TrackColumnLayout{
      .columns = {{.column = TrackColumn::Title, .visible = true}, {.column = TrackColumn::Tags, .visible = true}},
    };

    auto const columns = expandingTrackColumnsForLayout(layout);

    REQUIRE(columns.size() == 1);
    CHECK(columns.front() == TrackColumn::Tags);
  }

  TEST_CASE("TrackPresentation - expansion columns fall back to title", "[app][presentation]")
  {
    using namespace ao::rt;

    auto const* const preset = builtinTrackPresentationPreset("album-artists");
    REQUIRE(preset != nullptr);

    auto const layout = trackColumnLayoutForPresentation(preset->spec);
    auto const columns = expandingTrackColumnsForLayout(layout);

    REQUIRE(columns.size() == 1);
    CHECK(columns.front() == TrackColumn::Title);
  }

  TEST_CASE("TrackColumnController - setLayoutAndApply updates visibility", "[app][presentation]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.track_column_controller_test");
    auto columnView = Gtk::ColumnView{};
    auto layoutModel = TrackColumnLayoutModel{};
    auto controller = TrackColumnController{columnView, layoutModel};

    controller.setupColumns([](TrackColumnDefinition const&) { return Gtk::SignalListItemFactory::create(); });

    auto layout = TrackColumnLayout{
      .columns = {{.column = TrackColumn::Album, .visible = true},
                  {.column = TrackColumn::Title, .visible = true},
                  {.column = TrackColumn::Tags, .visible = false}},
    };

    controller.setLayoutAndApply(layout);

    CHECK(layoutModel.layout() == normalizeTrackColumnLayout(layout));
    CHECK(controller.visibilityModel()->isVisible(TrackColumn::Title) == true);
    CHECK(controller.visibilityModel()->isVisible(TrackColumn::Tags) == false);
  }

  TEST_CASE("TrackPresentation - redundantFieldToColumn mapping", "[app][presentation]")
  {
    using namespace ao::rt;

    CHECK(redundantFieldToColumn(TrackSortField::Artist) == TrackColumn::Artist);
    CHECK(redundantFieldToColumn(TrackSortField::Album) == TrackColumn::Album);
    CHECK(redundantFieldToColumn(TrackSortField::AlbumArtist) == TrackColumn::AlbumArtist);
    CHECK(redundantFieldToColumn(TrackSortField::Genre) == TrackColumn::Genre);
    CHECK(redundantFieldToColumn(TrackSortField::Composer) == TrackColumn::Composer);
    CHECK(redundantFieldToColumn(TrackSortField::Work) == TrackColumn::Work);
    CHECK(redundantFieldToColumn(TrackSortField::Year) == TrackColumn::Year);

    // Non-display fields should return nullopt
    CHECK_FALSE(redundantFieldToColumn(TrackSortField::Title).has_value());
    CHECK_FALSE(redundantFieldToColumn(TrackSortField::Duration).has_value());
    CHECK_FALSE(redundantFieldToColumn(TrackSortField::DiscNumber).has_value());
    CHECK_FALSE(redundantFieldToColumn(TrackSortField::TrackNumber).has_value());
  }

  TEST_CASE("TrackPresentation - editable trait", "[app][presentation]")
  {
    for (auto const& def : trackColumnDefinitions())
    {
      bool const expected =
        def.column == TrackColumn::Title || def.column == TrackColumn::Artist || def.column == TrackColumn::Album;
      CHECK(def.editable == expected);
    }
  }

  TEST_CASE("TrackPresentation - trackColumnForPresentationField mapping", "[app][presentation]")
  {
    using namespace ao::rt;

    CHECK(trackColumnForPresentationField(TrackPresentationField::Title) == TrackColumn::Title);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Artist) == TrackColumn::Artist);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Album) == TrackColumn::Album);
    CHECK(trackColumnForPresentationField(TrackPresentationField::AlbumArtist) == TrackColumn::AlbumArtist);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Genre) == TrackColumn::Genre);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Composer) == TrackColumn::Composer);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Work) == TrackColumn::Work);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Year) == TrackColumn::Year);
    CHECK(trackColumnForPresentationField(TrackPresentationField::DiscNumber) == TrackColumn::DiscNumber);
    CHECK(trackColumnForPresentationField(TrackPresentationField::TrackNumber) == TrackColumn::TrackNumber);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Duration) == TrackColumn::Duration);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Tags) == TrackColumn::Tags);
  }

  TEST_CASE("TrackPresentation - draggable trait", "[app][presentation]")
  {
    for (auto const& def : trackColumnDefinitions())
    {
      bool const expected =
        def.column == TrackColumn::Artist || def.column == TrackColumn::Album || def.column == TrackColumn::Genre;
      CHECK(def.draggable == expected);
    }
  }
} // namespace ao::gtk::test
