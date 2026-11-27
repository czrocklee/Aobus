// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/CustomPresentationEditorModel.h>

#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/ViewServiceTestSupport.h"
#include <ao/library/Credits.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("CustomPresentationEditorModel - edits custom presentation draft")
  {
    auto spec = rt::TrackPresentationSpec{.presetId = rt::BuiltinPreset::Albums};
    auto const customId = rt::CustomPresetId{1};
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
      CHECK(model.collectState(customId).spec.groupBy == rt::TrackGroupKey::Album);
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

      auto const state = model.collectState(customId);
      CHECK(state.label == "Artist View");
      CHECK(state.spec.isPreset(customId));
      CHECK(state.spec.groupBy == rt::TrackGroupKey::AlbumArtist);
    }

    SECTION("manages sort term rows")
    {
      REQUIRE(model.tryAddSortTerm());
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
      auto const originalGroupKey = model.collectState(customId).spec.groupBy;
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
      CHECK(model.collectState(customId).spec.groupBy == originalGroupKey);
      CHECK(std::vector<rt::TrackSortTerm>{model.sortTerms().begin(), model.sortTerms().end()} == originalSortTerms);
      CHECK(std::vector<rt::TrackField>{model.visibleFields().begin(), model.visibleFields().end()} ==
            originalVisibleFields);
    }

    SECTION("rejects the maximum sort row index without mutation")
    {
      auto const before = std::vector<rt::TrackSortTerm>{model.sortTerms().begin(), model.sortTerms().end()};

      CHECK_FALSE(model.tryMoveSortTermDown(std::numeric_limits<std::size_t>::max()));
      CHECK(std::vector<rt::TrackSortTerm>{model.sortTerms().begin(), model.sortTerms().end()} == before);
    }

    SECTION("rejects the maximum visible row index without mutation")
    {
      auto const before = std::vector<rt::TrackField>{model.visibleFields().begin(), model.visibleFields().end()};

      CHECK_FALSE(model.tryMoveVisibleFieldDown(std::numeric_limits<std::size_t>::max()));
      CHECK(std::vector<rt::TrackField>{model.visibleFields().begin(), model.visibleFields().end()} == before);
    }

    SECTION("keeps the first input sort term per field and normalizes visible fields on export")
    {
      auto const sourceId = rt::CustomPresetId{2};
      auto duplicateSpec = rt::TrackPresentationSpec{
        .presetId = sourceId,
        .groupBy = rt::TrackGroupKey::Album,
        .sortBy =
          {
            {.field = rt::TrackSortField::Artist, .ascending = true},
            {.field = rt::TrackSortField::Artist, .ascending = false},
          },
        .visibleFields = {rt::TrackField::Title, rt::TrackField::Album},
      };
      auto duplicateModel =
        CustomPresentationEditorModel{ao::test::englishMessageCatalog(), duplicateSpec, "Duplicate View"};
      duplicateModel.addVisibleField();
      duplicateModel.addVisibleField();
      auto const expectedDraftFields =
        std::vector{rt::TrackField::Title, rt::TrackField::Album, rt::TrackField::Title, rt::TrackField::Title};
      REQUIRE(std::vector<rt::TrackField>{
                duplicateModel.visibleFields().begin(), duplicateModel.visibleFields().end()} == expectedDraftFields);
      auto const expected = rt::CustomTrackPresentationPreset{
        .label = "Duplicate View",
        .optBasePresetId = rt::TrackPresetId{sourceId},
        .spec =
          {
            .presetId = customId,
            .groupBy = rt::TrackGroupKey::Album,
            .sortBy = {duplicateSpec.sortBy.front()},
            .visibleFields = {rt::TrackField::Title, rt::TrackField::Album},
          },
      };

      CHECK(duplicateModel.collectState(customId) == expected);
      CHECK(std::vector<rt::TrackSortTerm>{duplicateModel.sortTerms().begin(), duplicateModel.sortTerms().end()} ==
            expected.spec.sortBy);
      CHECK(std::vector<rt::TrackField>{duplicateModel.visibleFields().begin(), duplicateModel.visibleFields().end()} ==
            expectedDraftFields);
    }

    SECTION("defaults empty populated fields only in exported state")
    {
      auto const emptySpec = rt::TrackPresentationSpec{
        .presetId = rt::BuiltinPreset::ClassicalWorks,
        .groupBy = rt::TrackGroupKey::Work,
      };
      auto emptyModel = CustomPresentationEditorModel{ao::test::englishMessageCatalog(), emptySpec, "Empty View"};
      auto const expected = rt::CustomTrackPresentationPreset{
        .label = "Empty View",
        .optBasePresetId = rt::TrackPresetId{rt::BuiltinPreset::ClassicalWorks},
        .spec =
          {
            .presetId = customId,
            .groupBy = rt::TrackGroupKey::Work,
            .visibleFields = {rt::TrackField::Title},
          },
      };

      CHECK(emptyModel.collectState(customId) == expected);
      CHECK(emptyModel.sortTerms().empty());
      CHECK(emptyModel.visibleFields().empty());
    }

    SECTION("collects a presentation preset")
    {
      auto const state = model.collectState(customId);

      CHECK(state.label == "Album View");
      CHECK(state.optBasePresetId == rt::TrackPresetId{rt::BuiltinPreset::Albums});
      CHECK(state.spec.isPreset(customId));
      CHECK(state.spec.groupBy == rt::TrackGroupKey::Album);
      CHECK(state.spec.sortBy == spec.sortBy);
      CHECK(state.spec.visibleFields == spec.visibleFields);
    }

    SECTION("copying a sorted ungrouped view makes its effective sort the new preset's base sort")
    {
      spec.groupBy = rt::TrackGroupKey::None;
      spec.visibleFields.push_back(rt::TrackField::DisplayTrackNumber);
      spec.sortBy.push_back({.field = rt::TrackSortField::TrackNumber, .ascending = true});
      spec.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::DisplayTrackNumber, .ascending = false};
      model.populate(spec, "Sorted copy");
      auto const expectedSort = std::vector<rt::TrackSortTerm>{
        {.field = rt::TrackSortField::DiscNumber, .ascending = false},
        {.field = rt::TrackSortField::TrackNumber, .ascending = false},
        {.field = rt::TrackSortField::Artist, .ascending = true},
        {.field = rt::TrackSortField::Year, .ascending = false},
      };

      auto const state = model.collectState(customId);

      CHECK(state.optBasePresetId == rt::TrackPresetId{rt::BuiltinPreset::Albums});
      CHECK(state.spec.isPreset(customId));
      CHECK(state.spec.sortBy == expectedSort);
      CHECK(state.spec.visibleFields == spec.visibleFields);
      CHECK_FALSE(state.spec.optColumnSort);
    }
  }

  TEST_CASE("CustomPresentationEditorModel - an added Songs sort row saves and restores without duplicate keys")
  {
    auto const& songs = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Songs).spec;
    auto model = CustomPresentationEditorModel{ao::test::englishMessageCatalog(), songs, "Extended Songs"};
    REQUIRE(model.tryAddSortTerm());
    REQUIRE(model.sortTerms().size() == songs.sortBy.size() + 1);
    auto const optTitle = model.optionIndexForSortField(rt::TrackSortField::Title);
    REQUIRE(optTitle);
    CHECK_FALSE(model.trySetSortFieldByOptionIndex(songs.sortBy.size(), *optTitle));
    CHECK_FALSE(std::ranges::contains(songs.sortBy, model.sortTerms().back().field, &rt::TrackSortTerm::field));
    auto const preset = model.collectState(rt::CustomPresetId{1});
    auto tempDir = ao::test::TempDir{};

    {
      auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);
      REQUIRE(runtimePtr->workspace().addCustomPreset(preset));
      runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());
    }

    auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);
    REQUIRE(runtimePtr->workspace().restoreSession(runtimePtr->workspaceConfigStore()));
    auto const restoredPresets = runtimePtr->workspace().customPresets();
    REQUIRE(restoredPresets.size() == 1);
    CHECK(restoredPresets.front() == preset);
  }

  TEST_CASE("CustomPresentationEditorModel - removing a sort row frees its field for another row")
  {
    auto model = CustomPresentationEditorModel{ao::test::englishMessageCatalog()};
    auto const optionCount = model.sortFieldOptions().size();

    for (std::size_t i = 0; i < optionCount; ++i)
    {
      REQUIRE(model.tryAddSortTerm());
    }

    CHECK_FALSE(model.tryAddSortTerm());
    REQUIRE(model.sortTerms().size() == optionCount);
    auto const field = model.sortTerms().front().field;
    REQUIRE(model.tryRemoveSortTerm(0));
    REQUIRE(model.tryAddSortTerm());
    CHECK(model.sortTerms().back().field == field);
  }

  TEST_CASE("CustomPresentationEditorModel - saving a grouped copy preserves headings and row order")
  {
    using library::CreditKind;
    auto fixture = rt::test::ViewServiceFixture{};
    auto const baker = fixture.addTrack({.work = "Interlude", .credits = {{"Baker", CreditKind::Conductor, ""}}});
    auto const adamsFinale = fixture.addTrack({.work = "Finale", .credits = {{"Adams", CreditKind::Conductor, ""}}});
    auto const adamsAria = fixture.addTrack({.work = "Aria", .credits = {{"Adams", CreditKind::Conductor, ""}}});
    auto const viewId = fixture.requireView({
      .optPresentation = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::ClassicalConductors).spec,
    });
    auto expectedRows = std::array{adamsAria, adamsFinale, baker};

    SECTION("a descending grouping column cannot reverse the sections")
    {
      REQUIRE(fixture.service.setColumnSort(viewId, {.column = rt::TrackField::Conductor, .ascending = false}));
    }

    SECTION("a descending non-grouping column sorts the rows within each section")
    {
      REQUIRE(fixture.service.setColumnSort(viewId, {.column = rt::TrackField::Work, .ascending = false}));
      expectedRows = {adamsFinale, adamsAria, baker};
    }

    auto const checkDisplay = [&]
    {
      auto const projectionPtr = fixture.requireProjection(viewId);
      REQUIRE(projectionPtr->groupCount() == 2);
      auto const first = projectionPtr->groupAt(0);
      auto const second = projectionPtr->groupAt(1);
      CHECK(first.heading.primary == rt::TrackGroupHeadingValue{std::string{"Adams"}});
      CHECK(first.rows.count == 2);
      CHECK(second.heading.primary == rt::TrackGroupHeadingValue{std::string{"Baker"}});
      CHECK(second.rows.count == 1);
      REQUIRE(projectionPtr->size() == expectedRows.size());

      for (std::size_t i = 0; i < expectedRows.size(); ++i)
      {
        CHECK(projectionPtr->trackIdAt(i) == expectedRows[i]);
      }
    };

    checkDisplay();
    auto const liveSpec = fixture.service.trackListState(viewId).presentation;
    auto const model = CustomPresentationEditorModel{ao::test::englishMessageCatalog(), liveSpec, "Conductors Copy"};
    auto const presetId = rt::CustomPresetId{1};
    REQUIRE(fixture.workspace.addCustomPreset(model.collectState(presetId)));
    auto const appliedRes = fixture.workspace.setActivePresentation(rt::TrackPresetId{presetId});
    REQUIRE(appliedRes);
    CHECK_FALSE(appliedRes->optColumnSort);
    checkDisplay();
  }

  TEST_CASE("CustomPresentationEditorModel - columns sharing a sort key stay distinct only in the visible choices")
  {
    auto const model = CustomPresentationEditorModel{ao::test::englishMessageCatalog()};
    auto const options = model.sortFieldOptions();

    CHECK(std::ranges::count(options, rt::TrackSortField::TrackNumber, &TrackSortFieldOption::value) == 1);
    CHECK(std::ranges::count(options, rt::TrackSortField::Movement, &TrackSortFieldOption::value) == 1);
    CHECK(model.optionIndexForVisibleField(rt::TrackField::DisplayTrackNumber));
    CHECK(model.optionIndexForVisibleField(rt::TrackField::TrackNumber));
    CHECK(model.optionIndexForVisibleField(rt::TrackField::Movement));
    CHECK(model.optionIndexForVisibleField(rt::TrackField::MovementNumber));
  }
} // namespace ao::uimodel::test
