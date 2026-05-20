// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/track/TrackPresentation.h"

#include "app/linux-gtk/track/TrackFieldUi.h"
#include "runtime/TrackField.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace ao::gtk::test
{
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

  TEST_CASE("TrackPresentation - TrackColumnLayoutModel reset produces empty state", "[app][presentation]")
  {
    auto model = TrackColumnLayoutModel{};

    for (auto const& width : model.state().widths)
    {
      CHECK(width == 0);
    }

    model.reset();

    for (auto const& width : model.state().widths)
    {
      CHECK(width == 0);
    }
  }

  TEST_CASE("TrackPresentation - TrackColumnLayoutModel setState and signal", "[app][presentation]")
  {
    auto model = TrackColumnLayoutModel{};
    auto fired = false;

    model.signalChanged().connect([&fired] { fired = true; });

    auto state = TrackColumnViewState{};
    state.widths[static_cast<std::size_t>(rt::TrackField::Title)] = 200;
    model.setState(state);

    CHECK(fired);
    CHECK(model.state().widths[static_cast<std::size_t>(rt::TrackField::Title)] == 200);
  }

  TEST_CASE("TrackPresentation - TrackColumnViewState equality", "[app][presentation]")
  {
    auto a = TrackColumnViewState{};
    auto b = TrackColumnViewState{};

    CHECK(a == b);

    a.widths[static_cast<std::size_t>(rt::TrackField::Title)] = 100;
    CHECK(a != b);

    b.widths[static_cast<std::size_t>(rt::TrackField::Title)] = 100;
    CHECK(a == b);

    a.fieldOrder = {rt::TrackField::Album, rt::TrackField::Title};
    CHECK(a != b);
  }
} // namespace ao::gtk::test
