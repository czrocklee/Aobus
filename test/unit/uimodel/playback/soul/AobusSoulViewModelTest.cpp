// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/audio/Quality.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("AobusSoulViewModel - view state generation", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};

    auto log = ao::test::RenderLog<AobusSoulViewState>{};
    auto const viewModel = AobusSoulViewModel{fixture.playback, [&log](auto const& view) { log.render(view); }};

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

  TEST_CASE("AobusSoulViewModel - unchanged aura snapshots do not rerender", "[uimodel][regression][playback][soul]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto log = ao::test::RenderLog<AobusSoulViewState>{};
    auto const viewModel = AobusSoulViewModel{fixture.playback, [&log](auto const& view) { log.render(view); }};
    REQUIRE(log.states.size() == 1);
    auto const revisionBefore = fixture.playback.snapshot().revision;

    fixture.commands().setShuffleMode(ShuffleMode::On);

    CHECK(fixture.playback.snapshot().revision > revisionBefore);
    CHECK(log.states.size() == 1);
  }

  TEST_CASE("AobusSoul - brand tokens match the asset source of truth", "[uimodel][unit][playback][soul]")
  {
    CHECK(kAobusSoulBrandCyan == AobusSoulRgb{.red = 0x06, .green = 0xB6, .blue = 0xD4});
    CHECK(kAobusSoulUiCyan == AobusSoulRgb{.red = 0x00, .green = 0xE5, .blue = 0xFF});
    CHECK(kAobusSoulAnchorAmber == AobusSoulRgb{.red = 0xF9, .green = 0x73, .blue = 0x16});
    CHECK(aobusSoulAuraRgb(SoulAura::Dormant) == kAobusSoulUiCyan);
    CHECK(aobusSoulAuraRgb(SoulAura::Veiled) == AobusSoulRgb{.red = 0x6B, .green = 0x72, .blue = 0x80});
    CHECK(aobusSoulAuraRgb(SoulAura::Radiant) == AobusSoulRgb{.red = 0xA8, .green = 0x55, .blue = 0xF7});
    CHECK(aobusSoulAuraRgb(SoulAura::Flowing) == AobusSoulRgb{.red = 0x10, .green = 0xB9, .blue = 0x81});
    CHECK(aobusSoulAuraRgb(SoulAura::Turbulent) == AobusSoulRgb{.red = 0xF5, .green = 0x9E, .blue = 0x0B});
    CHECK(aobusSoulAuraRgb(SoulAura::Burning) == AobusSoulRgb{.red = 0xEF, .green = 0x44, .blue = 0x44});
    CHECK(kAobusSoulNightField == AobusSoulRgb{.red = 0x11, .green = 0x18, .blue = 0x27});
  }

  TEST_CASE("AobusSoul - motion recipe exposes the shared GTK and TUI timing phases", "[uimodel][unit][playback][soul]")
  {
    auto const initial = aobusSoulMotionAt(std::chrono::duration<double>{0.0});
    CHECK(initial.breath == Catch::Approx{0.5});
    CHECK(initial.rotationDegrees == Catch::Approx{0.0});
    CHECK(initial.rotationRadians == Catch::Approx{0.0});
    CHECK(initial.luminance == Catch::Approx{kAobusSoulOpacityBase});
    CHECK(initial.hueShiftDegrees == Catch::Approx{0.0});

    auto const widestStroke = aobusSoulMotionAt(kAobusSoulBreathingPeriod / 4.0);
    CHECK(widestStroke.breath == Catch::Approx{1.0});

    auto const quarterTurn = aobusSoulMotionAt(kAobusSoulRotationPeriod / 4.0);
    CHECK(quarterTurn.rotationDegrees == Catch::Approx{90.0});

    auto const dimmest = aobusSoulMotionAt(kAobusSoulOpacityPeriod * 3.0 / 4.0);
    CHECK(dimmest.luminance == Catch::Approx{kAobusSoulOpacityFloor});

    auto const maxHueShift = aobusSoulMotionAt(kAobusSoulHuePeriod / 4.0);
    CHECK(maxHueShift.hueShiftDegrees == Catch::Approx{kAobusSoulMaxHueShiftDegrees});
  }
} // namespace ao::uimodel::test
