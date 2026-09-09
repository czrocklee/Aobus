// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/CustomPresentationEditorModel.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("CustomPresentationEditorModel - edits custom presentation draft", "[uimodel][unit][library][presentation]")
  {
    auto spec = rt::TrackPresentationSpec{};
    spec.groupBy = rt::TrackGroupKey::Album;
    spec.sortBy = {
      {.field = rt::TrackSortField::Artist, .ascending = true},
      {.field = rt::TrackSortField::Year, .ascending = false},
    };
    spec.visibleFields = {rt::TrackField::Title, rt::TrackField::Album};

    auto model = CustomPresentationEditorModel{ao::test::englishMessageCatalog(), spec, "Album View"};

    SECTION("populates initial state and options")
    {
      CHECK(model.label() == "Album View");
      CHECK(model.collectState("test").spec.groupBy == rt::TrackGroupKey::Album);
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

      CHECK(model.trySetGroupKeyByOptionIndex(*optArtistIndex + 1));
      model.setLabel("Artist View");

      auto const state = model.collectState("custom-id-1");
      CHECK(state.label == "Artist View");
      CHECK(state.spec.id == "custom-id-1");
      CHECK(state.spec.groupBy == rt::TrackGroupKey::AlbumArtist);
    }

    SECTION("manages sort term rows")
    {
      model.addSortTerm();
      REQUIRE(model.sortTerms().size() == 3);
      CHECK(model.sortTerms()[2] == rt::TrackSortTerm{.field = rt::TrackSortField::Title, .ascending = true});

      REQUIRE(model.optionIndexForSortField(rt::TrackSortField::Duration));
      CHECK(model.trySetSortFieldByOptionIndex(2, *model.optionIndexForSortField(rt::TrackSortField::Duration)));
      CHECK(model.trySetSortAscending(2, false));
      CHECK(model.sortTerms()[2] == rt::TrackSortTerm{.field = rt::TrackSortField::Duration, .ascending = false});

      CHECK(model.tryMoveSortTermUp(2));
      CHECK(model.sortTerms()[1].field == rt::TrackSortField::Duration);

      CHECK(model.tryMoveSortTermDown(1));
      CHECK(model.sortTerms()[2].field == rt::TrackSortField::Duration);

      CHECK(model.tryRemoveSortTerm(1));
      REQUIRE(model.sortTerms().size() == 2);
      CHECK(model.sortTerms()[1].field == rt::TrackSortField::Duration);
    }

    SECTION("manages visible field rows")
    {
      model.addVisibleField();
      REQUIRE(model.visibleFields().size() == 3);
      CHECK(model.visibleFields()[2] == rt::TrackField::Title);

      REQUIRE(model.optionIndexForVisibleField(rt::TrackField::Quality));
      CHECK(model.trySetVisibleFieldByOptionIndex(2, *model.optionIndexForVisibleField(rt::TrackField::Quality)));
      CHECK(model.visibleFields()[2] == rt::TrackField::Quality);

      CHECK(model.tryMoveVisibleFieldUp(2));
      CHECK(model.visibleFields()[1] == rt::TrackField::Quality);

      CHECK(model.tryMoveVisibleFieldDown(1));
      CHECK(model.visibleFields()[2] == rt::TrackField::Quality);

      CHECK(model.tryRemoveVisibleField(0));
      REQUIRE(model.visibleFields().size() == 2);
      CHECK(model.visibleFields()[1] == rt::TrackField::Quality);

      CHECK(model.tryRemoveVisibleField(0));
      REQUIRE(model.visibleFields().size() == 1);
      CHECK_FALSE(model.tryRemoveVisibleField(0));
      REQUIRE(model.visibleFields().size() == 1);
      CHECK(model.visibleFields()[0] == rt::TrackField::Quality);
    }

    SECTION("rejects out-of-range row operations")
    {
      auto const originalLabel = model.label();
      auto const originalGroupKey = model.collectState("test").spec.groupBy;
      auto const originalSortTermsSpan = model.sortTerms();
      auto const originalVisibleFieldsSpan = model.visibleFields();
      auto const originalSortTerms =
        std::vector<rt::TrackSortTerm>{originalSortTermsSpan.begin(), originalSortTermsSpan.end()};
      auto const originalVisibleFields =
        std::vector<rt::TrackField>{originalVisibleFieldsSpan.begin(), originalVisibleFieldsSpan.end()};

      CHECK_FALSE(model.trySetGroupKeyByOptionIndex(model.groupOptions().size()));
      CHECK_FALSE(model.trySetSortFieldByOptionIndex(99, 0));
      CHECK_FALSE(model.trySetSortFieldByOptionIndex(0, model.sortFieldOptions().size()));
      CHECK_FALSE(model.trySetSortAscending(99, false));
      CHECK_FALSE(model.tryMoveSortTermUp(0));
      CHECK_FALSE(model.tryMoveSortTermDown(model.sortTerms().size()));
      CHECK_FALSE(model.tryRemoveSortTerm(model.sortTerms().size()));
      CHECK_FALSE(model.trySetVisibleFieldByOptionIndex(99, 0));
      CHECK_FALSE(model.trySetVisibleFieldByOptionIndex(0, model.visibleFieldOptions().size()));
      CHECK_FALSE(model.tryMoveVisibleFieldUp(0));
      CHECK_FALSE(model.tryMoveVisibleFieldDown(model.visibleFields().size()));
      CHECK_FALSE(model.tryRemoveVisibleField(model.visibleFields().size()));

      CHECK(model.label() == originalLabel);
      CHECK(model.collectState("test").spec.groupBy == originalGroupKey);
      CHECK(std::vector<rt::TrackSortTerm>{model.sortTerms().begin(), model.sortTerms().end()} == originalSortTerms);
      CHECK(std::vector<rt::TrackField>{model.visibleFields().begin(), model.visibleFields().end()} ==
            originalVisibleFields);
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
} // namespace ao::uimodel::test
