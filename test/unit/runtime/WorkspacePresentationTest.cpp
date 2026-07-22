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

  TEST_CASE("WorkspaceService - NewViewDefault applies to a newly created view",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);

    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = fixture.firstListId,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::NewViewDefault,
          .spec = albumsPreset->spec,
        },
    }));

    CHECK(runtime.views().trackListState(viewId).presentation == normalizeTrackPresentationSpec(albumsPreset->spec));
  }

  TEST_CASE("WorkspaceService - NewViewDefault preserves a reused view's exact presentation",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;
    auto const* songsPreset = builtinTrackPresentationPreset("songs");
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(songsPreset != nullptr);
    REQUIRE(albumsPreset != nullptr);
    auto const firstViewId = requireNavigation(runtime, fixture.firstListId);
    REQUIRE(runtime.workspace().setActivePresentation(songsPreset->spec));
    requireNavigation(runtime, fixture.secondListId);

    auto const reusedViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = fixture.firstListId,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::NewViewDefault,
          .spec = albumsPreset->spec,
        },
    }));

    CHECK(reusedViewId == firstViewId);
    CHECK(runtime.views().trackListState(reusedViewId).presentation ==
          normalizeTrackPresentationSpec(songsPreset->spec));

    requireBackNavigation(runtime);
    CHECK(runtime.views().trackListState(runtime.workspace().snapshot().activeViewId).listId == fixture.secondListId);
    requireForwardNavigation(runtime);
    auto const replayedState = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(replayedState.id == firstViewId);
    CHECK(replayedState.presentation == normalizeTrackPresentationSpec(songsPreset->spec));
  }

  TEST_CASE("WorkspaceService - NewViewDefault creates a plain view beside a filtered view",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;
    auto const* songsPreset = builtinTrackPresentationPreset("songs");
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(songsPreset != nullptr);
    REQUIRE(albumsPreset != nullptr);
    auto const filteredViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target =
        FilteredListTarget{
          .listId = fixture.firstListId,
          .filterExpression = "$title ~ \"Needle\"",
        },
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::Override,
          .spec = songsPreset->spec,
        },
    }));

    auto const plainViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = fixture.firstListId,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::NewViewDefault,
          .spec = albumsPreset->spec,
        },
    }));

    CHECK(plainViewId != filteredViewId);
    auto const filteredState = runtime.views().trackListState(filteredViewId);
    CHECK(filteredState.filterExpression == "$title ~ \"Needle\"");
    CHECK(filteredState.presentation == normalizeTrackPresentationSpec(songsPreset->spec));
    auto const plainState = runtime.views().trackListState(plainViewId);
    CHECK(plainState.filterExpression.empty());
    CHECK(plainState.presentation == normalizeTrackPresentationSpec(albumsPreset->spec));
  }

  TEST_CASE("WorkspaceService - Override changes a reused view's presentation",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    auto const firstViewId = requireNavigation(runtime, fixture.firstListId);

    auto const reusedViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = fixture.firstListId,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::Override,
          .spec = albumsPreset->spec,
        },
    }));

    CHECK(reusedViewId == firstViewId);
    CHECK(runtime.views().trackListState(reusedViewId).presentation ==
          normalizeTrackPresentationSpec(albumsPreset->spec));
  }

  TEST_CASE("WorkspaceService - setActivePresentation records presentation history",
            "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    REQUIRE(runtime.workspace().setActivePresentation(albumsPreset->spec));

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
    auto const beforeRepeated = runtime.workspace().snapshot();
    REQUIRE(runtime.workspace().setActivePresentation(albumsPreset->spec));
    CHECK(runtime.workspace().snapshot() == beforeRepeated);

    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation.id == "list-order");
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
  }

  TEST_CASE("WorkspaceService - setActivePresentation resolves preset ids", "[runtime][unit][workspace][presentation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    auto const spec = ao::test::requireValue(runtime.workspace().setActivePresentation("albums"));

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
    auto const presentation = ao::test::requireValue(runtime.workspace().setActivePresentation("custom1_id"));
    CHECK(presentation.groupBy == TrackGroupKey::Work);

    REQUIRE(runtime.workspace().removeCustomPreset("custom1_id"));
    CHECK(emitCount == 3);
    CHECK(runtime.workspace().customPresets().empty());
  }
} // namespace ao::rt::test
