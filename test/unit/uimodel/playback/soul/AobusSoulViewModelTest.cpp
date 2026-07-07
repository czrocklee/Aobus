// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/audio/Backend.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("AobusSoulViewModel - view state generation", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};
    auto playback = PlaybackService{executor, viewService, testLib.library(), notificationService};

    auto log = ao::test::RenderLog<AobusSoulViewState>{};
    auto const viewModel = AobusSoulViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial render when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().isBreathing == false);
      CHECK(log.last().aura == SoulAura::Dormant);
    }
  }

  TEST_CASE("AobusSoulViewModel - playback signal resolves to branded aura", "[uimodel][unit][playback][soul]")
  {
    CHECK(resolveSoulAura(false, true, rt::QualityState{.overall = audio::Quality::BitwisePerfect}) ==
          SoulAura::Dormant);
    CHECK(resolveSoulAura(true, false, rt::QualityState{.overall = audio::Quality::BitwisePerfect}) ==
          SoulAura::Veiled);
    CHECK(resolveSoulAura(true,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::BitwisePerfect,
                                           .overall = audio::Quality::BitwisePerfect}) == SoulAura::Radiant);
    CHECK(resolveSoulAura(true,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::LosslessPadded,
                                           .overall = audio::Quality::LosslessPadded}) == SoulAura::Flowing);
    CHECK(resolveSoulAura(true,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::LosslessFloat,
                                           .overall = audio::Quality::LosslessFloat}) == SoulAura::Flowing);
    CHECK(resolveSoulAura(true,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::LinearIntervention,
                                           .overall = audio::Quality::LinearIntervention}) == SoulAura::Turbulent);
    CHECK(resolveSoulAura(true,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::LossySource,
                                           .pipelineQuality = audio::Quality::BitwisePerfect,
                                           .overall = audio::Quality::LossySource}) == SoulAura::Veiled);
    CHECK(resolveSoulAura(true,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::BitwisePerfect,
                                           .overall = audio::Quality::BitwisePerfect,
                                           .fullyVerified = false}) == SoulAura::Veiled);
    CHECK(resolveSoulAura(true, true, rt::QualityState{.overall = audio::Quality::Clipped}) == SoulAura::Burning);
  }
} // namespace ao::uimodel::test
