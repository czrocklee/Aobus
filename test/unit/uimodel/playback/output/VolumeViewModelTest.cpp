// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/uimodel/playback/output/VolumeViewModel.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("VolumeViewModel - view state generation", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;

    auto log = ao::test::RenderLog<VolumeViewState>{};
    auto viewModel = VolumeViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial render")
    {
      CHECK(!log.empty());
    }

    SECTION("handleVolumeChanged delegates to playback")
    {
      viewModel.handleVolumeChanged(0.35F);
      CHECK(playbackTransport.state().volume.level == 0.35F);
      CHECK(log.last().isHardwareAssisted == false); // Initially false in mock
    }

    SECTION("volume and mute changes render button icon and tooltip state")
    {
      addReadyAudioProvider(playbackTransport);
      viewModel.handleVolumeChanged(0.5F);

      CHECK(log.last().visible == true);
      CHECK(log.last().volume == 0.5F);
      CHECK(log.last().muted == false);
      CHECK(log.last().indicatorKind == VolumeIndicatorKind::Medium);
      CHECK(log.last().tooltip == "Volume: 50%");

      viewModel.handleMutedChanged(true);

      CHECK(log.last().visible == true);
      CHECK(log.last().volume == 0.5F);
      CHECK(log.last().muted == true);
      CHECK(log.last().indicatorKind == VolumeIndicatorKind::Muted);
      CHECK(log.last().tooltip == "Volume: 50% (Muted)");
    }

    SECTION("Mute controls")
    {
      viewModel.handleMutedChanged(true);
      CHECK(playbackTransport.state().volume.muted == true);
      CHECK(log.last().muted == true);

      viewModel.toggleMuted();
      CHECK(playbackTransport.state().volume.muted == false);
      CHECK(log.last().muted == false);
    }

    SECTION("handleScroll changes volume and unmutes if increasing")
    {
      viewModel.handleVolumeChanged(0.5F);
      viewModel.handleMutedChanged(true);

      viewModel.handleScroll(-1.0); // scroll up
      CHECK(playbackTransport.state().volume.level == Catch::Approx{0.52F}.margin(0.001F));
      CHECK(playbackTransport.state().volume.muted == false); // Clears mute

      viewModel.handleScroll(1.0); // scroll down
      CHECK(playbackTransport.state().volume.level == Catch::Approx{0.50F}.margin(0.001F));
    }

    SECTION("relative adjustment clamps and follows shared mute policy")
    {
      viewModel.handleVolumeChanged(0.5F);
      viewModel.handleMutedChanged(true);

      viewModel.adjustVolume(-1.0F);
      CHECK(playbackTransport.state().volume.level == 0.0F);
      CHECK(playbackTransport.state().volume.muted == true);

      viewModel.adjustVolume(0.05F);
      CHECK(playbackTransport.state().volume.level == Catch::Approx{0.05F}.margin(0.001F));
      CHECK(playbackTransport.state().volume.muted == false);

      viewModel.adjustVolume(2.0F);
      CHECK(playbackTransport.state().volume.level == 1.0F);
    }
  }

  TEST_CASE("VolumeViewModel - math helpers", "[uimodel][unit][playback]")
  {
    double const kWidth = 100.0;

    SECTION("resolveVolumeOffset")
    {
      CHECK(VolumeViewModel::resolveVolumeOffset(0.0, 50.0, 0.5F) == 0.5F);
      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, 0.0) == 0.0F);
      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, 50.0) == 0.5F);
      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, 100.0) == 1.0F);

      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, 25.0, 0.5F) == 0.75F);
      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, -25.0, 0.5F) == 0.25F);

      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, 150.0) == 1.0F);
      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, -50.0) == 0.0F);
    }

    SECTION("resolveVolumeScroll")
    {
      CHECK(VolumeViewModel::resolveVolumeScroll(0.5F, 1.0) == Catch::Approx{0.48F}.margin(0.001F));
      CHECK(VolumeViewModel::resolveVolumeScroll(0.5F, -1.0) == Catch::Approx{0.52F}.margin(0.001F));
      CHECK(VolumeViewModel::resolveVolumeScroll(0.01F, 1.0) == Catch::Approx{0.0F}.margin(0.001F));
      CHECK(VolumeViewModel::resolveVolumeScroll(0.99F, -1.0) == Catch::Approx{1.0F}.margin(0.001F));
    }

    SECTION("resolveIndicatorKind")
    {
      CHECK(VolumeViewModel::resolveIndicatorKind(0.5F, true) == VolumeIndicatorKind::Muted);
      CHECK(VolumeViewModel::resolveIndicatorKind(0.0F, false) == VolumeIndicatorKind::Muted);
      CHECK(VolumeViewModel::resolveIndicatorKind(0.25F, false) == VolumeIndicatorKind::Low);
      CHECK(VolumeViewModel::resolveIndicatorKind(0.33F, false) == VolumeIndicatorKind::Low);
      CHECK(VolumeViewModel::resolveIndicatorKind(0.50F, false) == VolumeIndicatorKind::Medium);
      CHECK(VolumeViewModel::resolveIndicatorKind(0.66F, false) == VolumeIndicatorKind::Medium);
      CHECK(VolumeViewModel::resolveIndicatorKind(0.75F, false) == VolumeIndicatorKind::High);
      CHECK(VolumeViewModel::resolveIndicatorKind(1.00F, false) == VolumeIndicatorKind::High);
    }

    SECTION("resolveTooltip")
    {
      CHECK(VolumeViewModel::resolveTooltip(0.64F, false, false) == "Volume: 64%");
      CHECK(VolumeViewModel::resolveTooltip(0.64F, true, false) == "Volume: 64% (Muted)");
      CHECK(VolumeViewModel::resolveTooltip(0.64F, false, true) == "Volume: 64% (Hardware)");
    }
  }
} // namespace ao::uimodel::test
