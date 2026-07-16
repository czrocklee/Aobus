// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/runtime/WorkspaceTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>

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
    REQUIRE(runtime.workspace().setActivePresentation(albumsPreset->spec));

    CHECK(runtime.workspace().canGoBack() == true);
    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation.id == "list-order");
  }

  TEST_CASE("WorkspaceService - setActivePresentation rejects a missing active view",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    auto const result = runtime.workspace().setActivePresentation(albumsPreset->spec);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidState);
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

    REQUIRE(runtime.workspace().setActivePresentation(albumsPreset->spec));
    auto const repeated = ao::test::requireValue(runtime.workspace().setActivePresentation(albumsPreset->spec));
    CHECK(repeated.disposition == WorkspaceCommitDisposition::NoChange);

    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
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
    REQUIRE(runtime.workspace().setActivePresentation(albumsPreset->spec, {.recordHistory = false}));

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation.id == "albums");
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("WorkspaceService - setActivePresentation resolves preset ids", "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    auto const result = ao::test::requireValue(runtime.workspace().setActivePresentation("albums"));
    auto const& spec = result.presentation;

    CHECK(spec.id == "albums");
    CHECK(spec.groupBy == TrackGroupKey::Album);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation.id == "albums");
  }

  TEST_CASE("WorkspaceService - setActivePresentation rejects unknown ids", "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    auto const result = runtime.workspace().setActivePresentation("nonexistent");

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("WorkspaceService - preset resolution rejects unknown ids before checking focus",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    auto const result = runtime.workspace().setActivePresentation("non_existent_preset");

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
  }

  TEST_CASE("WorkspaceService - custom presets can be added, updated, selected, and removed",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    std::int32_t emitCount = 0;
    auto const sub = runtime.workspace().onChanged(
      [&](WorkspaceChanged const& changed)
      {
        if (changed.cause == WorkspaceChangeCause::Presets)
        {
          emitCount++;
        }
      });

    auto preset = CustomTrackPresentationPreset{};
    preset.label = "custom1";
    preset.spec.id = "custom1_id";
    preset.spec.groupBy = TrackGroupKey::Composer;

    REQUIRE(runtime.workspace().addCustomPreset(preset));
    CHECK(emitCount == 1);

    auto presets = runtime.workspace().customPresets();
    REQUIRE(presets.size() == 1);
    CHECK(presets[0].label == "custom1");

    preset.spec.groupBy = TrackGroupKey::Work;
    REQUIRE(runtime.workspace().addCustomPreset(preset));
    CHECK(emitCount == 2);
    presets = runtime.workspace().customPresets();
    REQUIRE(presets.size() == 1);
    CHECK(presets[0].spec.groupBy == TrackGroupKey::Work);

    requireNavigation(runtime, fixture.firstListId);
    auto const result = ao::test::requireValue(runtime.workspace().setActivePresentation("custom1_id"));
    CHECK(result.presentation.groupBy == TrackGroupKey::Work);

    REQUIRE(runtime.workspace().removeCustomPreset("custom1_id"));
    CHECK(emitCount == 3);
    CHECK(runtime.workspace().customPresets().empty());
  }
} // namespace ao::rt::test
