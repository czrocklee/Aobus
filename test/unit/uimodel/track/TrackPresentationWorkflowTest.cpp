// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/track/TrackPresentationTestSupport.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/track/TrackPresentationCatalog.h>
#include <ao/uimodel/track/TrackPresentationPreferenceStore.h>
#include <ao/uimodel/track/TrackPresentationWorkflow.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace ao::uimodel::track::test
{
  TEST_CASE("TrackPresentationWorkflow - renders disabled picker without an active view",
            "[uimodel][workflow][track][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationWorkflow{fixture.viewService,
                                              fixture.workspace,
                                              fixture.catalog,
                                              fixture.preferences,
                                              [&rendered](auto const& state) { rendered.push_back(state); }};

    workflow.refresh();
    auto const command = workflow.selectPresentation("albums");

    REQUIRE(rendered.size() == 1);
    CHECK_FALSE(rendered[0].enabled);
    CHECK(rendered[0].activeViewId == rt::kInvalidViewId);
    CHECK(rendered[0].activeListId == kInvalidListId);
    CHECK(rendered[0].label == "Presentation");
    CHECK_FALSE(command.shouldApply);
    CHECK(fixture.preferences.listPresentations().empty());
  }

  TEST_CASE("TrackPresentationWorkflow - selecting a valid presentation stores preference and returns captured spec",
            "[uimodel][workflow][track][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    fixture.workspace.navigateTo(rt::kAllTracksListId);
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationWorkflow{fixture.viewService,
                                              fixture.workspace,
                                              fixture.catalog,
                                              fixture.preferences,
                                              [&rendered](auto const& state) { rendered.push_back(state); }};

    workflow.refresh();
    auto const command = workflow.selectPresentation("albums");

    REQUIRE(rendered.size() == 2);
    CHECK(rendered[0].enabled);
    CHECK(rendered[0].activeListId == rt::kAllTracksListId);
    CHECK(rendered[0].activePresentationId == std::string{rt::kDefaultTrackPresentationId});
    REQUIRE(command.shouldApply);
    CHECK(command.spec.id == "albums");
    REQUIRE(fixture.preferences.presentationIdForList(rt::kAllTracksListId));
    CHECK(*fixture.preferences.presentationIdForList(rt::kAllTracksListId) == "albums");
    CHECK(rendered[1].activePresentationId == "albums");
    CHECK(rendered[1].label == fixture.catalog.labelForId("albums"));
  }

  TEST_CASE("TrackPresentationWorkflow - ignores unknown selections without changing preferences",
            "[uimodel][workflow][track][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    fixture.workspace.navigateTo(rt::kAllTracksListId);
    auto workflow = TrackPresentationWorkflow{
      fixture.viewService, fixture.workspace, fixture.catalog, fixture.preferences, [](auto const&) {}};

    auto const command = workflow.selectPresentation("not-a-presentation");

    CHECK_FALSE(command.shouldApply);
    CHECK(fixture.preferences.listPresentations().empty());
  }

  TEST_CASE("TrackPresentationWorkflow - active view presentation changes refresh picker state",
            "[uimodel][workflow][track][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    fixture.workspace.navigateTo(rt::kAllTracksListId);
    auto rendered = std::vector<TrackPresentationPickerState>{};
    auto workflow = TrackPresentationWorkflow{fixture.viewService,
                                              fixture.workspace,
                                              fixture.catalog,
                                              fixture.preferences,
                                              [&rendered](auto const& state) { rendered.push_back(state); }};
    auto const activeViewId = fixture.workspace.layoutState().activeViewId;
    auto const* const albums = rt::builtinTrackPresentationPreset("albums");
    REQUIRE(albums != nullptr);

    fixture.viewService.setPresentation(activeViewId, albums->spec);

    REQUIRE(rendered.size() == 1);
    CHECK(rendered[0].activePresentationId == "albums");
    CHECK(rendered[0].label == fixture.catalog.labelForId("albums"));
  }
} // namespace ao::uimodel::track::test
