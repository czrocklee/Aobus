// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/track/TrackCustomViewEditorModel.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::track::test
{
  TEST_CASE("TrackCustomViewEditorModel - edits custom presentation draft", "[uimodel][track][presentation]")
  {
    auto spec = rt::TrackPresentationSpec{};
    spec.groupBy = rt::TrackGroupKey::Album;
    spec.sortBy = {
      {.field = rt::TrackSortField::Artist, .ascending = true},
      {.field = rt::TrackSortField::Year, .ascending = false},
    };
    spec.visibleFields = {rt::TrackField::Title, rt::TrackField::Album};

    auto model = TrackCustomViewEditorModel{spec, "Album View"};

    SECTION("populates initial state and options")
    {
      CHECK(model.label() == "Album View");
      CHECK(model.groupKey() == rt::TrackGroupKey::Album);
      REQUIRE(model.groupKeyOptionIndex());
      CHECK(model.groupOptions()[*model.groupKeyOptionIndex()].label == "Album");

      REQUIRE(model.optionIndexForSortField(rt::TrackSortField::Year));
      CHECK(model.sortFieldOptions()[*model.optionIndexForSortField(rt::TrackSortField::Year)].label == "Year");

      REQUIRE(model.optionIndexForVisibleField(rt::TrackField::Album));
      CHECK(model.visibleFieldOptions()[*model.optionIndexForVisibleField(rt::TrackField::Album)].label == "Album");
    }

    SECTION("edits top-level metadata")
    {
      auto const optArtistIndex = model.groupKeyOptionIndex();
      REQUIRE(optArtistIndex);

      CHECK(model.setGroupKeyByOptionIndex(*optArtistIndex + 1));
      model.setLabel("Artist View");

      auto const state = model.collectState("custom-id-1");
      CHECK(state.label == "Artist View");
      CHECK(state.spec.id == "custom-id-1");
      CHECK(state.spec.groupBy == rt::TrackGroupKey::AlbumArtist);
    }

    SECTION("supports direct group key updates")
    {
      model.setGroupKey(rt::TrackGroupKey::Work);
      CHECK(model.groupKey() == rt::TrackGroupKey::Work);
      REQUIRE(model.groupKeyOptionIndex());
      CHECK(model.groupOptions()[*model.groupKeyOptionIndex()].label == "Work");

      model.setGroupKey(static_cast<rt::TrackGroupKey>(250));
      CHECK_FALSE(model.groupKeyOptionIndex());
    }

    SECTION("manages sort term rows")
    {
      model.addSortTerm();
      REQUIRE(model.sortTerms().size() == 3);
      CHECK(model.sortTerms()[2] == rt::TrackSortTerm{.field = rt::TrackSortField::Title, .ascending = true});

      REQUIRE(model.optionIndexForSortField(rt::TrackSortField::Duration));
      CHECK(model.setSortFieldByOptionIndex(2, *model.optionIndexForSortField(rt::TrackSortField::Duration)));
      CHECK(model.setSortAscending(2, false));
      CHECK(model.sortTerms()[2] == rt::TrackSortTerm{.field = rt::TrackSortField::Duration, .ascending = false});

      CHECK(model.moveSortTermUp(2));
      CHECK(model.sortTerms()[1].field == rt::TrackSortField::Duration);

      CHECK(model.moveSortTermDown(1));
      CHECK(model.sortTerms()[2].field == rt::TrackSortField::Duration);

      CHECK(model.removeSortTerm(1));
      REQUIRE(model.sortTerms().size() == 2);
      CHECK(model.sortTerms()[1].field == rt::TrackSortField::Duration);
    }

    SECTION("manages visible field rows")
    {
      model.addVisibleField();
      REQUIRE(model.visibleFields().size() == 3);
      CHECK(model.visibleFields()[2] == rt::TrackField::Title);

      REQUIRE(model.optionIndexForVisibleField(rt::TrackField::Quality));
      CHECK(model.setVisibleFieldByOptionIndex(2, *model.optionIndexForVisibleField(rt::TrackField::Quality)));
      CHECK(model.visibleFields()[2] == rt::TrackField::Quality);

      CHECK(model.moveVisibleFieldUp(2));
      CHECK(model.visibleFields()[1] == rt::TrackField::Quality);

      CHECK(model.moveVisibleFieldDown(1));
      CHECK(model.visibleFields()[2] == rt::TrackField::Quality);

      CHECK(model.removeVisibleField(0));
      REQUIRE(model.visibleFields().size() == 2);
      CHECK(model.visibleFields()[1] == rt::TrackField::Quality);
    }

    SECTION("rejects out-of-range row operations")
    {
      CHECK_FALSE(model.setGroupKeyByOptionIndex(model.groupOptions().size()));
      CHECK_FALSE(model.setSortFieldByOptionIndex(99, 0));
      CHECK_FALSE(model.setSortFieldByOptionIndex(0, model.sortFieldOptions().size()));
      CHECK_FALSE(model.setSortAscending(99, false));
      CHECK_FALSE(model.moveSortTermUp(0));
      CHECK_FALSE(model.moveSortTermDown(model.sortTerms().size()));
      CHECK_FALSE(model.removeSortTerm(model.sortTerms().size()));
      CHECK_FALSE(model.setVisibleFieldByOptionIndex(99, 0));
      CHECK_FALSE(model.setVisibleFieldByOptionIndex(0, model.visibleFieldOptions().size()));
      CHECK_FALSE(model.moveVisibleFieldUp(0));
      CHECK_FALSE(model.moveVisibleFieldDown(model.visibleFields().size()));
      CHECK_FALSE(model.removeVisibleField(model.visibleFields().size()));
    }

    SECTION("collects a presentation preset")
    {
      auto const state = model.collectState("generated-custom-id");

      CHECK(state.label == "Album View");
      CHECK(state.spec.id == "generated-custom-id");
      CHECK(state.spec.groupBy == rt::TrackGroupKey::Album);
      CHECK(state.spec.sortBy == spec.sortBy);
      CHECK(state.spec.visibleFields == spec.visibleFields);
    }
  }
} // namespace ao::uimodel::track::test
