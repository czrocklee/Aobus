// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackColumnLayouts - orders visible fields using stored order first")
  {
    auto const visible = std::vector{rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album};
    auto const stored = std::vector{rt::TrackField::Album, rt::TrackField::DiscNumber, rt::TrackField::Title};

    auto const ordered = visibleTrackFieldsInStoredOrder(visible, stored);

    REQUIRE(ordered.size() == 3);
    CHECK(ordered[0] == rt::TrackField::Album);
    CHECK(ordered[1] == rt::TrackField::Title);
    CHECK(ordered[2] == rt::TrackField::Artist);
  }

  TEST_CASE("TrackColumnLayouts - stored visibility filters presentation fields without hiding new fields")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .visibleFields = {rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album},
    };
    auto const stored = std::vector{
      TrackColumnState{.field = rt::TrackField::Album, .weight = 1.0, .visibility = TrackColumnVisibility::Hide},
      TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.0, .visibility = TrackColumnVisibility::Show},
    };

    auto const visible = visibleTrackFieldsInStoredLayout(presentation, stored);

    REQUIRE(visible.size() == 2);
    CHECK(visible[0] == rt::TrackField::Artist);
    CHECK(visible[1] == rt::TrackField::Title);
  }

  TEST_CASE("TrackColumnLayouts - switching presentations can leave every available column hidden")
  {
    using Field = rt::TrackField;
    auto const wide = rt::TrackPresentationSpec{.visibleFields = {Field::Title, Field::Artist}};
    auto const narrow = rt::TrackPresentationSpec{.visibleFields = {Field::Title}};
    auto const stored = std::vector{
      TrackColumnState{.field = Field::Title, .visibility = TrackColumnVisibility::Hide},
    };

    CHECK(visibleTrackFieldsInStoredLayout(wide, stored) == std::vector{Field::Artist});
    CHECK(visibleTrackFieldsInStoredLayout(narrow, stored).empty());
  }

  TEST_CASE("TrackColumnLayouts - Albums offers Album without showing it until explicitly requested")
  {
    using Field = rt::TrackField;
    auto const* preset = rt::builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);
    auto const& presentation = preset->spec;
    auto stored = std::vector{TrackColumnState{.field = Field::Album, .weight = 1.75}};

    auto const choices = trackColumnChoices(presentation, stored);
    auto const album = std::ranges::find(choices, Field::Album, &TrackColumnChoice::field);
    REQUIRE(album != choices.end());
    CHECK_FALSE(album->visible);
    auto const defaultColumns = std::vector{Field::DisplayTrackNumber, Field::Title, Field::Artist, Field::Duration};
    CHECK(visibleTrackFieldsInStoredLayout(presentation, {}) == defaultColumns);
    CHECK(visibleTrackFieldsInStoredLayout(presentation, stored) == defaultColumns);

    stored[0].visibility = TrackColumnVisibility::Show;
    CHECK(visibleTrackFieldsInStoredLayout(presentation, stored) ==
          std::vector{Field::Album, Field::DisplayTrackNumber, Field::Title, Field::Artist, Field::Duration});

    stored[0].visibility = TrackColumnVisibility::Hide;
    CHECK(visibleTrackFieldsInStoredLayout(presentation, stored) == defaultColumns);
  }

  TEST_CASE("TrackColumnLayouts - visibility-only toggles do not give a column a stored position")
  {
    using Field = rt::TrackField;
    auto const* const albums = rt::builtinTrackPresentationPreset("albums");
    auto const* const songs = rt::builtinTrackPresentationPreset("songs");
    REQUIRE(albums);
    REQUIRE(songs);
    auto stored = std::vector{
      TrackColumnState{.field = Field::Album, .visibility = TrackColumnVisibility::Show},
    };

    CHECK(visibleTrackFieldsInStoredLayout(albums->spec, stored) ==
          std::vector{Field::DisplayTrackNumber, Field::Title, Field::Artist, Field::Duration, Field::Album});
    CHECK(visibleTrackFieldsInStoredLayout(songs->spec, stored) == songs->spec.visibleFields);

    stored[0].visibility = TrackColumnVisibility::Hide;
    CHECK(visibleTrackFieldsInStoredLayout(albums->spec, stored) == albums->spec.visibleFields);
    CHECK(visibleTrackFieldsInStoredLayout(songs->spec, stored) ==
          std::vector{Field::Title, Field::Artist, Field::Duration, Field::Year});

    stored[0].visibility = TrackColumnVisibility::Show;
    CHECK(visibleTrackFieldsInStoredLayout(songs->spec, stored) == songs->spec.visibleFields);
  }

  TEST_CASE("TrackColumnLayouts - offers equivalent secondary grouping fields but not other row facts")
  {
    using Field = rt::TrackField;
    using Group = rt::TrackGroupKey;
    auto presentation = rt::TrackPresentationSpec{.visibleFields = {Field::Title}};
    auto secondary = Field::AlbumArtist;

    SECTION("Albums offers Album Artist but not the row's Artist or Year")
    {
      presentation.groupBy = Group::Album;
      presentation.redundantFields = {Field::AlbumArtist, Field::Artist, Field::Year};
    }

    SECTION("Works offers Composer but not the row's Album Artist")
    {
      presentation.groupBy = Group::Work;
      presentation.redundantFields = {Field::Composer, Field::AlbumArtist};
      secondary = Field::Composer;
    }

    auto const choices = trackColumnChoices(presentation, {});
    REQUIRE(choices.size() == 2);
    CHECK(choices[0].field == Field::Title);
    CHECK(choices[0].visible);
    CHECK(choices[1].field == secondary);
    CHECK_FALSE(choices[1].visible);

    auto const shown = std::vector{
      TrackColumnState{.field = secondary, .weight = 1.0, .visibility = TrackColumnVisibility::Show},
    };
    CHECK(visibleTrackFieldsInStoredLayout(presentation, shown) == std::vector{secondary, Field::Title});
  }

  TEST_CASE("TrackColumnLayouts - a custom presentation keeps authored columns and excludes unrelated fields")
  {
    using Field = rt::TrackField;
    auto const presentation = rt::TrackPresentationSpec{
      .id = "custom",
      .groupBy = rt::TrackGroupKey::Album,
      .visibleFields = {Field::Title, Field::Album},
      .redundantFields = {Field::Album, Field::Tags},
    };
    auto const stored = std::vector{
      TrackColumnState{.field = Field::AlbumArtist, .weight = 1.5, .visibility = TrackColumnVisibility::Show},
      TrackColumnState{.field = Field::Tags, .weight = 2.0, .visibility = TrackColumnVisibility::Show},
    };

    auto const choices = trackColumnChoices(presentation, stored);
    REQUIRE(choices.size() == 2);
    CHECK(choices[0].field == Field::Title);
    CHECK(choices[1].field == Field::Album);
    CHECK(choices[1].visible);
    CHECK(visibleTrackFieldsInStoredLayout(presentation, stored) == std::vector{Field::Title, Field::Album});
  }

  TEST_CASE("TrackColumnLayouts - a credit heading does not grant availability for the count-bearing column")
  {
    using Field = rt::TrackField;
    auto const presentation = rt::TrackPresentationSpec{
      .groupBy = rt::TrackGroupKey::Conductor,
      .visibleFields = {Field::Title},
      .redundantFields = {Field::Conductor},
    };
    auto const choices = trackColumnChoices(presentation, {});

    REQUIRE(choices.size() == 1);
    CHECK(choices[0].field == Field::Title);
  }

  TEST_CASE("TrackColumnLayouts - a geometry edit positions only participating overrides")
  {
    using Field = rt::TrackField;
    auto const hiddenGenre =
      TrackColumnState{.field = Field::Genre, .weight = 1.25, .visibility = TrackColumnVisibility::Hide};
    auto const hiddenDuration = TrackColumnState{.field = Field::Duration, .visibility = TrackColumnVisibility::Hide};
    auto const stored = std::vector{
      TrackColumnState{.field = Field::Album, .visibility = TrackColumnVisibility::Show},
      hiddenGenre,
      TrackColumnState{.field = Field::Artist, .weight = 1.75},
      hiddenDuration,
    };
    auto const specs = std::vector{
      TrackColumnSolveSpec{.field = Field::Artist, .weight = 1.75},
      TrackColumnSolveSpec{.field = Field::Album, .weight = 2.25},
    };

    auto const merged = mergeVisibleTrackColumnSpecs(stored, specs);

    auto const expected = std::vector{
      hiddenGenre,
      TrackColumnState{.field = Field::Artist, .weight = 1.75},
      hiddenDuration,
      TrackColumnState{.field = Field::Album, .weight = 2.25, .visibility = TrackColumnVisibility::Show},
    };
    CHECK(merged == expected);
    auto const presentation = rt::TrackPresentationSpec{.visibleFields = {Field::Album, Field::Artist, Field::Title}};
    CHECK(visibleTrackFieldsInStoredLayout(presentation, merged) ==
          std::vector{Field::Artist, Field::Album, Field::Title});
  }

  TEST_CASE("TrackColumnLayouts - geometry updates preserve column visibility choices by field")
  {
    auto const hiddenGenre =
      TrackColumnState{.field = rt::TrackField::Genre, .weight = 1.25, .visibility = TrackColumnVisibility::Hide};
    auto const unavailableAlbum =
      TrackColumnState{.field = rt::TrackField::Album, .weight = 1.5, .visibility = TrackColumnVisibility::Show};
    auto const stored = std::vector{
      TrackColumnState{.field = rt::TrackField::Title, .weight = 1.0},
      hiddenGenre,
      TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.0, .visibility = TrackColumnVisibility::Show},
      unavailableAlbum,
    };
    auto const visibleSpecs = std::vector{
      TrackColumnSolveSpec{.field = rt::TrackField::Artist, .weight = 2.1254},
      TrackColumnSolveSpec{.field = rt::TrackField::Title},
      TrackColumnSolveSpec{.field = rt::TrackField::Duration, .fixedWidth = 95},
    };

    auto const merged = mergeVisibleTrackColumnSpecs(stored, visibleSpecs);

    REQUIRE(merged.size() == 5);
    CHECK(merged[0] == TrackColumnState{
                         .field = rt::TrackField::Artist, .weight = 2.125, .visibility = TrackColumnVisibility::Show});
    CHECK(merged[1] == hiddenGenre);
    CHECK(merged[2] == TrackColumnState{.field = rt::TrackField::Title, .weight = 3.0});
    CHECK(merged[3] == unavailableAlbum);
    CHECK(merged[4] == TrackColumnState{.field = rt::TrackField::Duration, .width = 95});
  }
} // namespace ao::uimodel::test
