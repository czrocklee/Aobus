// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/library/presentation/TrackPresentationTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackPresentationPickerViewModel - renders disabled picker without an active view",
            "[uimodel][unit][workflow]")
  {
    auto fixture = TrackPresentationFixture{};
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.preferences,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};

    workflow.refresh();
    auto const optCommand = workflow.selectPresentation("albums");

    REQUIRE(rendered.size() == 1);
    CHECK_FALSE(rendered[0].enabled);
    CHECK(rendered[0].activeViewId == rt::kInvalidViewId);
    CHECK(rendered[0].label == "Presentation");
    CHECK_FALSE(optCommand);
    CHECK(fixture.preferences.listPresentations().empty());
  }

  TEST_CASE("TrackPresentationPickerViewModel - selecting a presentation captures the spec without persisting",
            "[uimodel][unit][workflow]")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.preferences,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};

    workflow.refresh();
    auto const optCommand = workflow.selectPresentation("albums");

    REQUIRE(rendered.size() == 1);
    CHECK(rendered[0].enabled);
    CHECK(rendered[0].label == fixture.catalog.labelForId(rt::kDefaultTrackPresentationId));
    REQUIRE(optCommand);
    CHECK(optCommand->targetViewId == fixture.workspace.snapshot().activeViewId);
    CHECK(optCommand->targetListId == rt::kAllTracksListId);
    CHECK(optCommand->spec.id == "albums");
    CHECK(fixture.preferences.listPresentations().empty());
  }

  TEST_CASE("TrackPresentationPickerViewModel - completing an applied selection persists the list preference",
            "[uimodel][unit][workflow]")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.preferences,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};

    auto const optCommand = workflow.selectPresentation("albums");
    REQUIRE(optCommand);

    REQUIRE(fixture.viewService.setPresentation(optCommand->targetViewId, optCommand->spec));
    workflow.completeSelection(*optCommand);

    REQUIRE(fixture.preferences.presentationIdForList(rt::kAllTracksListId));
    CHECK(*fixture.preferences.presentationIdForList(rt::kAllTracksListId) == "albums");
    REQUIRE_FALSE(rendered.empty());
    CHECK(rendered.back().label == fixture.catalog.labelForId("albums"));
  }

  // A selection that never reaches completeSelection stands for a runtime apply
  // that failed or was superseded before the deferred apply ran.
  TEST_CASE("TrackPresentationPickerViewModel - an unapplied selection leaves preference and label unchanged",
            "[uimodel][unit][regression][workflow]")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.preferences,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};

    REQUIRE(workflow.selectPresentation("albums"));

    workflow.refresh();

    CHECK(fixture.preferences.listPresentations().empty());
    REQUIRE_FALSE(rendered.empty());
    CHECK(rendered.back().label == fixture.catalog.labelForId(rt::kDefaultTrackPresentationId));
  }

  TEST_CASE("TrackPresentationPickerViewModel - a closed active view rejects selection without optimistic state",
            "[uimodel][unit][regression][workflow]")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.preferences,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};
    auto const activeViewId = fixture.workspace.snapshot().activeViewId;
    REQUIRE(fixture.workspace.closeView(activeViewId));
    rendered.clear();

    auto const optCommand = workflow.selectPresentation("albums");

    CHECK_FALSE(optCommand);
    CHECK(rendered.empty());
    CHECK(fixture.preferences.listPresentations().empty());
  }

  TEST_CASE("TrackPresentationPickerViewModel - ignores unknown selections without changing preferences",
            "[uimodel][unit][workflow]")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto workflow = TrackPresentationPickerViewModel{
      fixture.viewService, fixture.workspace, fixture.catalog, fixture.preferences, [](auto const&) {}};

    auto const optCommand = workflow.selectPresentation("not-a-presentation");

    CHECK_FALSE(optCommand);
    CHECK(fixture.preferences.listPresentations().empty());
  }

  TEST_CASE("TrackPresentationPickerViewModel - active view presentation changes refresh picker state",
            "[uimodel][unit][workflow]")
  {
    auto fixture = TrackPresentationFixture{};
    REQUIRE(fixture.workspace.navigate({.target = rt::kAllTracksListId}));
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationPickerViewModel{fixture.viewService,
                                                     fixture.workspace,
                                                     fixture.catalog,
                                                     fixture.preferences,
                                                     [&rendered](auto const& state) { rendered.push_back(state); }};
    auto const activeViewId = fixture.workspace.snapshot().activeViewId;
    auto const* const albums = rt::builtinTrackPresentationPreset("albums");
    REQUIRE(albums != nullptr);

    REQUIRE(fixture.viewService.setPresentation(activeViewId, albums->spec));

    REQUIRE(rendered.size() == 1);
    CHECK(rendered[0].label == fixture.catalog.labelForId("albums"));
  }
} // namespace ao::uimodel::test
