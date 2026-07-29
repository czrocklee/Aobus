// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/ApplicationPlaybackTestSupport.h"
#include "test/unit/runtime/PlaybackUiTestSupport.h"
#include <ao/audio/Quality.h>
#include <ao/audio/Transport.h>
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
      CHECK(log.last().motionMode == AobusSoulMotionMode::Dormant);
      CHECK(log.last().aura == SoulAura::Dormant);
    }
  }

  TEST_CASE("AobusSoulViewModel - playback signal resolves to branded aura", "[uimodel][unit][playback][soul]")
  {
    CHECK(resolveSoulAura(audio::Transport::Idle, true, rt::QualityState{.overall = audio::Quality::BitwisePerfect}) ==
          SoulAura::Dormant);
    CHECK(resolveSoulAura(audio::Transport::Playing,
                          false,
                          rt::QualityState{.overall = audio::Quality::BitwisePerfect}) == SoulAura::Veiled);
    CHECK(resolveSoulAura(audio::Transport::Playing,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::BitwisePerfect,
                                           .overall = audio::Quality::BitwisePerfect}) == SoulAura::Radiant);
    CHECK(resolveSoulAura(audio::Transport::Playing,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::LosslessPadded,
                                           .overall = audio::Quality::LosslessPadded}) == SoulAura::Flowing);
    CHECK(resolveSoulAura(audio::Transport::Playing,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::LosslessFloat,
                                           .overall = audio::Quality::LosslessFloat}) == SoulAura::Flowing);
    CHECK(resolveSoulAura(audio::Transport::Playing,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::LinearIntervention,
                                           .overall = audio::Quality::LinearIntervention}) == SoulAura::Turbulent);
    CHECK(resolveSoulAura(audio::Transport::Playing,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::LossySource,
                                           .pipelineQuality = audio::Quality::BitwisePerfect,
                                           .overall = audio::Quality::LossySource}) == SoulAura::Veiled);
    CHECK(resolveSoulAura(audio::Transport::Playing,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::BitwisePerfect,
                                           .overall = audio::Quality::BitwisePerfect,
                                           .fullyVerified = false}) == SoulAura::Veiled);
    CHECK(resolveSoulAura(audio::Transport::Playing, true, rt::QualityState{.overall = audio::Quality::Clipped}) ==
          SoulAura::Burning);
  }

  TEST_CASE("AobusSoulViewModel - paused playback freezes motion and keeps the live quality aura",
            "[uimodel][regression][playback][soul]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const trackId = fixture.addPlayableTrack("Paused Soul");
    auto& playback = fixture.runtime.playback();
    auto log = ao::test::RenderLog<AobusSoulViewState>{};
    auto const viewModel = AobusSoulViewModel{playback, [&log](auto const& view) { log.render(view); }};
    REQUIRE(fixture.playFromView(trackId));
    REQUIRE(log.last().motionMode == AobusSoulMotionMode::Animating);
    auto const playingAura = log.last().aura;

    playback.commands().pause();

    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Paused);
    CHECK(log.last().motionMode == AobusSoulMotionMode::Frozen);
    CHECK(log.last().aura == playingAura);
  }

  TEST_CASE("resolveSoulAura uses live quality while paused", "[uimodel][regression][playback][soul]")
  {
    CHECK(resolveSoulAura(audio::Transport::Paused,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::BitwisePerfect,
                                           .overall = audio::Quality::BitwisePerfect}) == SoulAura::Radiant);
    CHECK(resolveSoulAura(audio::Transport::Paused,
                          true,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::LinearIntervention,
                                           .overall = audio::Quality::LinearIntervention}) == SoulAura::Turbulent);
    CHECK(resolveSoulAura(audio::Transport::Paused,
                          false,
                          rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                           .pipelineQuality = audio::Quality::BitwisePerfect,
                                           .overall = audio::Quality::BitwisePerfect}) == SoulAura::Veiled);
  }

  TEST_CASE("AobusSoulViewModel - unchanged aura snapshots do not rerender", "[uimodel][regression][playback][soul]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto log = ao::test::RenderLog<AobusSoulViewState>{};
    auto const viewModel = AobusSoulViewModel{fixture.playback, [&log](auto const& view) { log.render(view); }};
    REQUIRE(log.states.size() == 1);
    fixture.commands().setShuffleMode(ShuffleMode::On);

    CHECK(fixture.playback.snapshot().succession.shuffle == ShuffleMode::On);
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

  TEST_CASE("AobusSoul - gradient keeps a cyan core and counter-shifted quality body",
            "[uimodel][unit][playback][soul]")
  {
    auto const stationary = aobusSoulGradientColors(kAobusSoulTurbulent, 0.0);
    CHECK(stationary.core == AobusSoulRgb{.red = 0x00, .green = 0xE5, .blue = 0xFF});
    CHECK(stationary.body == AobusSoulRgb{.red = 0xF5, .green = 0x9E, .blue = 0x0B});

    auto const flowing = aobusSoulGradientColors(kAobusSoulTurbulent, 10.0);
    CHECK(flowing.core == AobusSoulRgb{.red = 0x00, .green = 0xBA, .blue = 0xFF});
    CHECK(flowing.body == AobusSoulRgb{.red = 0xF5, .green = 0x77, .blue = 0x0B});
  }

  TEST_CASE("AobusSoul - geometry is one immutable cross-frontend recipe", "[uimodel][unit][playback][soul]")
  {
    CHECK(kAobusSoulGeometry.referenceHeight == 65.0);
    CHECK(kAobusSoulGeometry.radius == 30.0);
    CHECK(kAobusSoulGeometry.anchorStrokeWidth == 10.0);
    CHECK(kAobusSoulGeometry.baseStrokeWidth == 9.0);
    CHECK(kAobusSoulGeometry.innerGlyphRadius == 14.0);
    CHECK(kAobusSoulGeometry.innerGlyphBarOffset == 5.6);
    CHECK(kAobusSoulGeometry.innerGlyphBarHalfHeight == 9.8);
    CHECK(kAobusSoulGeometry.logoCenterOffset == 43.5);
    CHECK(kAobusSoulCoreGradientStop == 0.382);
  }

  TEST_CASE("AobusSoul - motion mode follows transport state", "[uimodel][unit][playback][soul]")
  {
    CHECK(aobusSoulMotionMode(audio::Transport::Playing) == AobusSoulMotionMode::Animating);
    CHECK(aobusSoulMotionMode(audio::Transport::Paused) == AobusSoulMotionMode::Frozen);
    CHECK(aobusSoulMotionMode(audio::Transport::Idle) == AobusSoulMotionMode::Dormant);
    CHECK(aobusSoulMotionMode(audio::Transport::Seeking) == AobusSoulMotionMode::Dormant);

    CHECK(shouldAnimateAobusSoul(AobusSoulMotionMode::Animating, true, false));
    CHECK_FALSE(shouldAnimateAobusSoul(AobusSoulMotionMode::Frozen, true, false));
    CHECK_FALSE(shouldAnimateAobusSoul(AobusSoulMotionMode::Dormant, true, false));
    CHECK_FALSE(shouldAnimateAobusSoul(AobusSoulMotionMode::Animating, false, false));
    CHECK_FALSE(shouldAnimateAobusSoul(AobusSoulMotionMode::Animating, true, true));
  }

  TEST_CASE("AobusSoul - frozen animation retains the exact sampled frame while aura changes",
            "[uimodel][regression][playback][soul]")
  {
    auto animation = AobusSoulAnimationState{};
    animation.setMotionMode(AobusSoulMotionMode::Animating);
    animation.advance(kAobusSoulHuePeriod / 4.0);
    auto const animated = animation.visualFrame(kAobusSoulTurbulent);

    animation.setMotionMode(AobusSoulMotionMode::Frozen);
    animation.advance(std::chrono::seconds{5});
    auto const frozen = animation.visualFrame(kAobusSoulTurbulent);

    CHECK(frozen == animated);

    auto const recolored = animation.visualFrame(kAobusSoulRadiant);
    CHECK(recolored.motion == frozen.motion);
    CHECK(recolored.gradientColors == aobusSoulGradientColors(kAobusSoulRadiant, frozen.motion.hueShiftDegrees));
    CHECK_FALSE(recolored.gradientColors.body == frozen.gradientColors.body);

    auto const frozenElapsed = animation.elapsed();
    animation.setMotionMode(AobusSoulMotionMode::Animating);
    animation.advance(std::chrono::milliseconds{20});
    CHECK(animation.elapsed() > frozenElapsed);
    CHECK_FALSE(animation.motionFrame() == frozen.motion);

    animation.setMotionMode(AobusSoulMotionMode::Dormant);
    CHECK(animation.elapsed() == std::chrono::duration<double>::zero());
    CHECK(animation.motionFrame() == AobusSoulMotionFrame{});
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
