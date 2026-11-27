// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/TestFixtureSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/WorkspaceTestSupport.h"
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

  TEST_CASE("WorkspaceService - NewViewDefault applies to a newly created view")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;

    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = fixture.listIds().first,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::NewViewDefault,
          .spec = albums,
        },
    }));

    CHECK(runtime.views().trackListState(viewId).presentation == normalizeTrackPresentationSpec(albums));
  }

  TEST_CASE("WorkspaceService - NewViewDefault preserves a reused view's exact presentation")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto songs = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;
    auto const firstViewId = requireNavigation(runtime, fixture.listIds().first);
    REQUIRE(runtime.workspace().setActivePresentation(songs));
    auto const duration = TrackColumnSort{TrackField::Duration, false};
    REQUIRE(runtime.views().setColumnSort(firstViewId, duration));
    songs.optColumnSort = duration;
    requireNavigation(runtime, fixture.listIds().second);

    auto const reusedViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = fixture.listIds().first,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::NewViewDefault,
          .spec = albums,
        },
    }));

    CHECK(reusedViewId == firstViewId);
    CHECK(runtime.views().trackListState(reusedViewId).presentation == normalizeTrackPresentationSpec(songs));

    requireBackNavigation(runtime);
    CHECK(runtime.views().trackListState(runtime.workspace().snapshot().activeViewId).listId ==
          fixture.listIds().second);
    requireForwardNavigation(runtime);
    auto const replayedState = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(replayedState.id == firstViewId);
    CHECK(replayedState.presentation == normalizeTrackPresentationSpec(songs));
  }

  TEST_CASE("WorkspaceService - NewViewDefault creates a plain view beside a filtered view")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const& songs = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;
    auto const filteredViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target =
        FilteredListTarget{
          .listId = fixture.listIds().first,
          .filterExpression = "$title ~ \"Needle\"",
        },
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::Override,
          .spec = songs,
        },
    }));

    auto const plainViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = fixture.listIds().first,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::NewViewDefault,
          .spec = albums,
        },
    }));

    CHECK(plainViewId != filteredViewId);
    auto const filteredState = runtime.views().trackListState(filteredViewId);
    CHECK(filteredState.filterExpression == "$title ~ \"Needle\"");
    CHECK(filteredState.presentation == normalizeTrackPresentationSpec(songs));
    auto const plainState = runtime.views().trackListState(plainViewId);
    CHECK(plainState.filterExpression.empty());
    CHECK(plainState.presentation == normalizeTrackPresentationSpec(albums));
  }

  TEST_CASE("WorkspaceService - Override changes a reused view's presentation")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;
    auto const firstViewId = requireNavigation(runtime, fixture.listIds().first);

    auto const reusedViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = fixture.listIds().first,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::Override,
          .spec = albums,
        },
    }));

    CHECK(reusedViewId == firstViewId);
    CHECK(runtime.views().trackListState(reusedViewId).presentation == normalizeTrackPresentationSpec(albums));
  }

  TEST_CASE("WorkspaceService - setActivePresentation records presentation history")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);

    REQUIRE(runtime.workspace().setActivePresentation(builtinTrackPresentationPreset(BuiltinPreset::Albums).spec));

    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation.isPreset(BuiltinPreset::Library));
  }

  TEST_CASE("WorkspaceService - setActivePresentation rejects a missing active view")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const res =
      runtime.workspace().setActivePresentation(builtinTrackPresentationPreset(BuiltinPreset::Albums).spec);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("WorkspaceService - setActivePresentation deduplicates the current spec")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;
    requireNavigation(runtime, fixture.listIds().first);

    REQUIRE(runtime.workspace().setActivePresentation(albums));
    auto const beforeRepeated = runtime.workspace().snapshot();
    REQUIRE(runtime.workspace().setActivePresentation(albums));
    CHECK(runtime.workspace().snapshot() == beforeRepeated);

    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation.isPreset(BuiltinPreset::Library));
  }

  TEST_CASE("WorkspaceService - setActivePresentation can skip recording history")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);

    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;
    REQUIRE_FALSE(runtime.workspace().canGoBack());
    REQUIRE(runtime.workspace().setActivePresentation(albums, {.recordHistory = false}));
    CHECK_FALSE(runtime.workspace().canGoBack());

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation.isPreset(BuiltinPreset::Albums));
  }

  TEST_CASE("WorkspaceService - setActivePresentation resolves preset ids")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    auto const spec = ao::test::requireValue(runtime.workspace().setActivePresentation(BuiltinPreset::Albums));

    CHECK(spec.isPreset(BuiltinPreset::Albums));
    CHECK(spec.groupBy == TrackGroupKey::Album);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation == spec);
  }

  TEST_CASE("WorkspaceService - reselecting the active preset clears its column sort")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const viewId = requireNavigation(runtime, fixture.listIds().first);
    auto presetId = TrackPresetId{};

    SECTION("built-in preset")
    {
      presetId = BuiltinPreset::Songs;
    }

    SECTION("custom preset")
    {
      auto const id = CustomPresetId{7};
      auto preset = CustomTrackPresentationPreset{.label = "Years first"};
      preset.spec = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
      preset.spec.presetId = id;
      preset.spec.sortBy = {{TrackSortField::Year, false}, {TrackSortField::Title, true}};
      REQUIRE(workspace.addCustomPreset(preset));
      presetId = id;
    }

    auto const base = requireValue(workspace.setActivePresentation(presetId));
    auto const term = TrackColumnSort{TrackField::Duration, false};
    REQUIRE(runtime.views().setColumnSort(viewId, term));
    auto sorted = base;
    sorted.optColumnSort = term;
    REQUIRE(runtime.views().trackListState(viewId).presentation == sorted);

    REQUIRE(workspace.setActivePresentation(presetId));

    auto const reset = runtime.views().trackListState(viewId);
    CHECK(reset.presentation == base);
    CHECK(reset.sortBy == base.sortBy);
    REQUIRE(requireBackNavigation(runtime) == viewId);
    CHECK(runtime.views().trackListState(viewId).presentation == sorted);
    REQUIRE(requireForwardNavigation(runtime) == viewId);
    CHECK(runtime.views().trackListState(viewId).presentation == base);
  }

  TEST_CASE("WorkspaceService - a full presentation cannot set an unsortable column override")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const viewId = requireNavigation(runtime, fixture.listIds().first);
    auto const before = workspace.snapshot();
    auto const original = runtime.views().trackListState(viewId).presentation;
    auto invalid = original;
    invalid.optColumnSort = TrackColumnSort{TrackField::MovementTotal};
    std::int32_t workspaceChanges = 0;
    auto const sub = workspace.onChanged([&](WorkspaceChanged const&) noexcept { ++workspaceChanges; });

    SECTION("replacing the active view's presentation")
    {
      auto const res = workspace.setActivePresentation(invalid);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
    }

    SECTION("opening a new view with an explicit presentation")
    {
      auto const res = workspace.navigate({
        .target = fixture.listIds().second,
        .optPresentation = NavigationPresentation{.spec = invalid},
      });
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
    }

    settleRuntimeCallbacks(runtime);
    CHECK(runtime.views().trackListState(viewId).presentation == original);
    CHECK(workspace.snapshot() == before);
    CHECK_FALSE(workspace.canGoBack());
    CHECK(workspaceChanges == 0);
  }

  TEST_CASE("WorkspaceService - setActivePresentation rejects unknown ids")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    auto const res = runtime.workspace().setActivePresentation(CustomPresetId{42});

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
  }

  TEST_CASE("WorkspaceService - preset resolution rejects unknown ids before checking focus")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const res = runtime.workspace().setActivePresentation(CustomPresetId{42});

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
  }

  TEST_CASE("WorkspaceService - custom presets can be added, updated, selected, and removed")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    std::int32_t emitCount = 0;
    auto const sub = runtime.workspace().onChanged(
      [&](WorkspaceChanged const& changed) noexcept
      {
        if (changed.cause == WorkspaceChangeCause::Presets)
        {
          emitCount++;
        }
      });

    auto preset = CustomTrackPresentationPreset{};
    preset.label = "custom1";
    preset.spec.presetId = CustomPresetId{0};
    preset.spec.groupBy = TrackGroupKey::Composer;

    REQUIRE(runtime.workspace().addCustomPreset(preset));
    settleRuntimeCallbacks(runtime);
    CHECK(emitCount == 1);

    auto presets = runtime.workspace().customPresets();
    REQUIRE(presets.size() == 1);
    CHECK(presets[0].label == "custom1");

    preset.spec.groupBy = TrackGroupKey::Work;
    REQUIRE(runtime.workspace().addCustomPreset(preset));
    settleRuntimeCallbacks(runtime);
    CHECK(emitCount == 2);
    presets = runtime.workspace().customPresets();
    REQUIRE(presets.size() == 1);
    CHECK(presets[0].spec.groupBy == TrackGroupKey::Work);

    requireNavigation(runtime, fixture.listIds().first);
    auto const presentation = ao::test::requireValue(runtime.workspace().setActivePresentation(CustomPresetId{0}));
    CHECK(presentation.groupBy == TrackGroupKey::Work);

    REQUIRE(runtime.workspace().removeCustomPreset(CustomPresetId{0}));
    settleRuntimeCallbacks(runtime);
    CHECK(emitCount == 3);
    CHECK(runtime.workspace().customPresets().empty());
  }

  TEST_CASE("WorkspaceService - rejects invalid custom preset definitions before publication")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& workspace = fixture.runtime().workspace();
    auto preset = CustomTrackPresentationPreset{.label = "Title order"};

    SECTION("view-local column sort")
    {
      preset.spec.presetId = CustomPresetId{1};
      preset.spec.optColumnSort = TrackColumnSort{TrackField::Title, false};
    }

    SECTION("built-in preset id")
    {
      preset.spec.presetId = BuiltinPreset::Library;
    }

    SECTION("duplicate base keys with different directions")
    {
      preset.spec.presetId = CustomPresetId{1};
      preset.spec.sortBy = {
        {TrackSortField::Title, true}, {TrackSortField::Artist, true}, {TrackSortField::Title, false}};
    }

    auto const res = workspace.addCustomPreset(preset);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(workspace.customPresets().empty());
  }

  TEST_CASE("WorkspaceService - custom preset identity is its spec id rather than its label")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& workspace = fixture.runtime().workspace();
    auto first = CustomTrackPresentationPreset{};
    first.label = "Shared label";
    first.spec.presetId = CustomPresetId{1};
    first.spec.groupBy = TrackGroupKey::Composer;

    REQUIRE(workspace.addCustomPreset(first));
    first.label = "Renamed label";
    REQUIRE(workspace.addCustomPreset(first));

    auto presets = workspace.customPresets();
    REQUIRE(presets.size() == 1);
    CHECK(presets.front().label == "Renamed label");
    CHECK(presets.front().presetId() == CustomPresetId{1});

    auto second = first;
    second.spec.presetId = CustomPresetId{2};
    REQUIRE(workspace.addCustomPreset(second));

    presets = workspace.customPresets();
    REQUIRE(presets.size() == 2);
    CHECK(presets[0].presetId() == CustomPresetId{1});
    CHECK(presets[1].presetId() == CustomPresetId{2});
    CHECK(presets[0].label == presets[1].label);
  }
} // namespace ao::rt::test
