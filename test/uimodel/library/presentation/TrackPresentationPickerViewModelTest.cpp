// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>

#include "test/MessageCatalogTestSupport.h"
#include "test/uimodel/library/presentation/TrackPresentationTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackPresentationPickerViewModel - localizes shared eligibility copy")
  {
    auto const textCatalog = ao::test::messageCatalog("de-DE");
    auto const eligibility = trackPresentationEligibility(textCatalog, rt::kAllTracksListId, "list-order");

    CHECK_FALSE(eligibility.enabled);
    CHECK(eligibility.disabledReason.contains("gespeicherte Liste"));
  }

  TEST_CASE("TrackPresentationPickerViewModel - renders disabled picker without an active view")
  {
    auto fixture = TrackPresentationFixture{};
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.listPresentations,
                                                     fixture.textCatalog,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};

    workflow.refresh();
    auto const optCommand = workflow.selectPresentation("albums");

    REQUIRE(rendered.size() == 1);
    CHECK_FALSE(rendered[0].enabled);
    CHECK(rendered[0].activeViewId == rt::kInvalidViewId);
    CHECK(rendered[0].label == "Presentation");
    CHECK_FALSE(optCommand);
    CHECK(fixture.listPresentations.snapshot().empty());
  }

  TEST_CASE("TrackPresentationPickerViewModel - selecting a presentation captures the spec without persisting")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.listPresentations,
                                                     fixture.textCatalog,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};

    workflow.refresh();
    auto const optCommand = workflow.selectPresentation("albums");

    REQUIRE(rendered.size() == 1);
    CHECK(rendered[0].enabled);
    CHECK(rendered[0].label == fixture.catalog.labelFor(rt::BuiltinPreset::Library));
    REQUIRE(optCommand);
    CHECK(optCommand->targetViewId == fixture.workspace.snapshot().activeViewId);
    CHECK(optCommand->targetListId == rt::kAllTracksListId);
    CHECK(optCommand->spec.isPreset(rt::BuiltinPreset::Albums));
    CHECK(fixture.listPresentations.snapshot().empty());
  }

  TEST_CASE("TrackPresentationPickerViewModel - All Tracks exposes Manual Order as disabled with its reason")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.listPresentations,
                                                     fixture.textCatalog,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};

    workflow.refresh();

    REQUIRE(rendered.size() == 1);
    auto const item = std::ranges::find(
      rendered.front().menuItems, rt::builtinPresetKey(rt::BuiltinPreset::ListOrder), &TrackPresentationMenuItem::id);
    REQUIRE(item != rendered.front().menuItems.end());
    CHECK_FALSE(item->enabled);
    CHECK(item->disabledReason ==
          "All Tracks has no saved order. Create a saved List to arrange the full library manually.");
    CHECK_FALSE(workflow.selectPresentation("list-order"));
    CHECK(fixture.listPresentations.snapshot().empty());
  }

  TEST_CASE("TrackPresentationPickerViewModel - completing an applied custom selection saves its preset id")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto const customId = rt::CustomPresetId{0x1a};
    fixture.catalog.addCustomPresentation({
      .label = "Title audit",
      .spec = {.presetId = customId, .visibleFields = {rt::TrackField::Title}},
    });
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.listPresentations,
                                                     fixture.textCatalog,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};

    auto const optCommand = workflow.selectPresentation("000000000000001a");
    REQUIRE(optCommand);
    REQUIRE(optCommand->spec.isPreset(customId));
    REQUIRE(optCommand->targetViewId == fixture.workspace.snapshot().activeViewId);
    REQUIRE(fixture.workspace.setActivePresentation(optCommand->spec, {.recordHistory = false}));
    workflow.completeSelection(*optCommand);

    CHECK(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId) == rt::TrackPresetId{customId});
    REQUIRE(rendered.size() == 1);
    CHECK(rendered.back().label == "Title audit");
    auto const columnSort = rt::TrackColumnSort{.column = rt::TrackField::Title, .ascending = false};

    REQUIRE(fixture.viewService.setColumnSort(optCommand->targetViewId, columnSort));

    REQUIRE(rendered.size() == 2);
    CHECK(rendered.back().label == "Title audit");
  }

  // A selection that never reaches completeSelection stands for a runtime apply
  // that failed or was superseded before the deferred apply ran.
  TEST_CASE("TrackPresentationPickerViewModel - an unapplied selection leaves preference and label unchanged")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.listPresentations,
                                                     fixture.textCatalog,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};

    REQUIRE(workflow.selectPresentation("albums"));

    workflow.refresh();

    CHECK(fixture.listPresentations.snapshot().empty());
    REQUIRE_FALSE(rendered.empty());
    CHECK(rendered.back().label == fixture.catalog.labelFor(rt::BuiltinPreset::Library));
  }

  TEST_CASE("TrackPresentationPickerViewModel - closing the active view rejects selection without optimistic state")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.listPresentations,
                                                     fixture.textCatalog,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};
    workflow.refresh();
    REQUIRE_FALSE(rendered.empty());
    REQUIRE(rendered.back().enabled);
    REQUIRE(workflow.selectPresentation("albums"));
    auto const activeViewId = fixture.workspace.snapshot().activeViewId;
    REQUIRE(fixture.workspace.closeView(activeViewId));
    REQUIRE(fixture.workspace.snapshot().activeViewId == rt::kInvalidViewId);
    REQUIRE_FALSE(fixture.viewService.findTrackListState(activeViewId));
    rendered.clear();

    auto const optCommand = workflow.selectPresentation("albums");

    CHECK_FALSE(optCommand);
    CHECK(rendered.empty());
    CHECK(fixture.listPresentations.snapshot().empty());
  }

  TEST_CASE("TrackPresentationPickerViewModel - ignores unknown selections without changing preferences")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.listPresentations,
                                                     fixture.textCatalog,
                                                     [](auto const&) {}};

    auto const optCommand = workflow.selectPresentation("not-a-presentation");

    CHECK_FALSE(optCommand);
    CHECK(fixture.listPresentations.snapshot().empty());
  }

  TEST_CASE("TrackPresentationPickerViewModel - a sorted view with a missing custom preset shows its key")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.listPresentations,
                                                     fixture.textCatalog,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};
    auto const unknown = rt::TrackPresentationSpec{
      .presetId = rt::CustomPresetId{0x2b},
      .optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::Title, .ascending = false},
      .visibleFields = {rt::TrackField::Title},
    };

    REQUIRE(fixture.workspace.setActivePresentation(unknown, {.recordHistory = false}));

    REQUIRE(rendered.size() == 1);
    CHECK(rendered[0].label == "000000000000002b");
  }

  TEST_CASE("TrackPresentationPickerViewModel - reselecting a preset clears only its view-local sort")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto const& albums = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Albums);
    REQUIRE(fixture.workspace.setActivePresentation(albums.spec, {.recordHistory = false}));
    auto const pinnedPreset = rt::TrackPresetId{rt::BuiltinPreset::Artists};
    fixture.listPresentations.setPresentationIdForList(rt::kAllTracksListId, pinnedPreset);
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.listPresentations,
                                                     fixture.textCatalog,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};
    workflow.refresh();
    auto const viewId = fixture.workspace.snapshot().activeViewId;
    auto const columnSort = rt::TrackColumnSort{.column = rt::TrackField::DisplayTrackNumber, .ascending = false};

    REQUIRE(fixture.viewService.setColumnSort(viewId, columnSort));

    REQUIRE(rendered.size() == 2);
    CHECK(rendered.back().label == "Albums");
    CHECK(rendered.back().menuItems == rendered.front().menuItems);
    CHECK(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId) == pinnedPreset);
    REQUIRE(fixture.viewService.trackListState(viewId).presentation.optColumnSort == columnSort);

    auto const optReselect = workflow.selectPresentation("albums");
    REQUIRE(optReselect);
    CHECK(optReselect->spec == albums.spec);
    REQUIRE(fixture.workspace.setActivePresentation(optReselect->spec, {.recordHistory = false}));
    workflow.completeSelection(*optReselect);

    CHECK(fixture.viewService.trackListState(viewId).presentation == albums.spec);
    CHECK(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId) == albums.spec.presetId);
    CHECK(rendered.back().label == "Albums");
  }
} // namespace ao::uimodel::test
