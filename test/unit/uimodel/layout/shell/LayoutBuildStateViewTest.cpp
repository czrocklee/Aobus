// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/shell/LayoutRuntimeState.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace ao::uimodel::test
{
  namespace
  {
    LayoutRuntimeState liveState()
    {
      auto state = LayoutRuntimeState{};
      state.activePresetId = "classic";
      state.componentState.preset = "classic";
      state.componentStateGeneration = 3;
      return state;
    }
  } // namespace

  TEST_CASE("LayoutBuildStateView - the live view tracks the carrier", "[uimodel][unit][layout][shell]")
  {
    auto runtimeState = liveState();
    auto const view = LayoutBuildStateView{runtimeState};

    CHECK(view.presetId() == "classic");
    CHECK(view.generation() == 3);
    CHECK_FALSE(view.isEditMode());
    CHECK(&view.document() == &runtimeState.componentState);

    // Later carrier mutations are observed, because the view borrows rather than copies.
    runtimeState.activePresetId = "modern";
    runtimeState.componentStateGeneration = 4;
    runtimeState.editMode = true;

    CHECK(view.presetId() == "modern");
    CHECK(view.generation() == 4);
    CHECK(view.isEditMode());
  }

  TEST_CASE("LayoutBuildStateView - a candidate view is independent of the carrier", "[uimodel][unit][layout][shell]")
  {
    auto runtimeState = liveState();
    auto const candidateDoc = LayoutComponentStateDocument{};

    auto const view = LayoutBuildStateView{"modern", candidateDoc, 9, true};

    CHECK(view.presetId() == "modern");
    CHECK(view.generation() == 9);
    CHECK(view.isEditMode());
    CHECK(&view.document() == &candidateDoc);

    runtimeState.activePresetId = "other";
    runtimeState.componentStateGeneration = 42;

    CHECK(view.presetId() == "modern");
    CHECK(view.generation() == 9);
  }

  TEST_CASE("LayoutBuildStateView - overriding pins the generation on either view", "[uimodel][unit][layout][shell]")
  {
    auto runtimeState = liveState();

    SECTION("a live view stops tracking the carrier once pinned")
    {
      auto view = LayoutBuildStateView{runtimeState};
      REQUIRE(view.generation() == 3);

      view.overrideGeneration(11);
      CHECK(view.generation() == 11);

      runtimeState.componentStateGeneration = 5;
      CHECK(view.generation() == 11);

      // Identity fields keep tracking the carrier; only the generation is pinned.
      runtimeState.activePresetId = "modern";
      CHECK(view.presetId() == "modern");
    }

    SECTION("a candidate view replaces its pinned generation")
    {
      auto const candidateDoc = LayoutComponentStateDocument{};
      auto view = LayoutBuildStateView{"modern", candidateDoc, 9};

      view.overrideGeneration(12);
      CHECK(view.generation() == 12);
    }
  }

  TEST_CASE("LayoutBuildStateView - the live view exposes the carrier's node-moved callback",
            "[uimodel][unit][layout][shell]")
  {
    auto runtimeState = liveState();
    auto moved = std::string{};

    runtimeState.editMode = true;
    runtimeState.onNodeMoved = [&moved](std::string const& nodeId, std::int32_t, std::int32_t) { moved = nodeId; };

    auto const view = LayoutBuildStateView{runtimeState};
    REQUIRE(view.onNodeMoved());

    view.onNodeMoved()("playback-bar", 10, 20);
    CHECK(moved == "playback-bar");
  }
} // namespace ao::uimodel::test
