// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/WorkspaceTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("WorkspaceService - setActivePresentation records presentation history",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    runtime.workspace().setActivePresentation(albumsPreset->spec);

    CHECK(runtime.workspace().canGoBack() == true);
    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.presentation.id == "list-order");
  }

  TEST_CASE("WorkspaceService - setActivePresentation is safe without an active view",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    REQUIRE_NOTHROW(runtime.workspace().setActivePresentation(albumsPreset->spec));
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("WorkspaceService - setActivePresentation deduplicates the current spec",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    requireNavigation(runtime, fixture.firstListId);

    runtime.workspace().setActivePresentation(albumsPreset->spec);
    runtime.workspace().setActivePresentation(albumsPreset->spec);

    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.presentation.id == "list-order");
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("WorkspaceService - setActivePresentation can skip recording history",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    runtime.workspace().setActivePresentation(albumsPreset->spec, {.recordHistory = false});

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.presentation.id == "albums");
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("WorkspaceService - setActivePresentation resolves preset ids", "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    auto const spec = runtime.workspace().setActivePresentation("albums");

    CHECK(spec.id == "albums");
    CHECK(spec.groupBy == TrackGroupKey::Album);
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.presentation.id == "albums");
  }

  TEST_CASE("WorkspaceService - setActivePresentation returns empty spec for unknown ids",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    auto const spec = runtime.workspace().setActivePresentation("nonexistent");

    CHECK(spec.id.empty());
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("WorkspaceService - setActivePresentation returns empty spec without an active view",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    auto const spec = runtime.workspace().setActivePresentation("non_existent_preset");

    CHECK(spec.id.empty());
  }

  TEST_CASE("WorkspaceService - custom presets can be added, updated, selected, and removed",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    std::int32_t emitCount = 0;
    auto const sub = runtime.workspace().onCustomPresetsChanged([&] { emitCount++; });

    auto preset = CustomTrackPresentationPreset{};
    preset.label = "custom1";
    preset.spec.id = "custom1_id";
    preset.spec.groupBy = TrackGroupKey::Composer;

    runtime.workspace().addCustomPreset(preset);
    CHECK(emitCount == 1);

    auto presets = runtime.workspace().customPresets();
    REQUIRE(presets.size() == 1);
    CHECK(presets[0].label == "custom1");

    preset.spec.groupBy = TrackGroupKey::Work;
    runtime.workspace().addCustomPreset(preset);
    CHECK(emitCount == 2);
    presets = runtime.workspace().customPresets();
    REQUIRE(presets.size() == 1);
    CHECK(presets[0].spec.groupBy == TrackGroupKey::Work);

    requireNavigation(runtime, fixture.firstListId);
    auto const spec = runtime.workspace().setActivePresentation("custom1_id");
    CHECK(spec.groupBy == TrackGroupKey::Work);

    runtime.workspace().removeCustomPreset("custom1_id");
    CHECK(emitCount == 3);
    CHECK(runtime.workspace().customPresets().empty());
  }
} // namespace ao::rt::test
