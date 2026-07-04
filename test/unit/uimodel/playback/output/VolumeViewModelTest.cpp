// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("VolumeViewModel - view state generation", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};
    auto playback = PlaybackService{executor, viewService, testLib.library(), notificationService};

    auto log = ao::test::RenderLog<VolumeViewState>{};
    auto viewModel = VolumeViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial render")
    {
      CHECK(!log.empty());
    }

    SECTION("handleVolumeChanged delegates to playback")
    {
      viewModel.handleVolumeChanged(0.35F);
      CHECK(playback.state().volume == 0.35F);
      CHECK(log.last().isHardwareAssisted == false); // Initially false in mock
    }

    SECTION("volume and mute changes render button icon and tooltip state")
    {
      addReadyAudioProvider(playback);
      viewModel.handleVolumeChanged(0.5F);

      CHECK(log.last().visible == true);
      CHECK(log.last().volume == 0.5F);
      CHECK(log.last().muted == false);
      CHECK(log.last().iconName == "audio-volume-medium-symbolic");
      CHECK(log.last().tooltip == "Volume: 50%");

      viewModel.handleMutedChanged(true);

      CHECK(log.last().visible == true);
      CHECK(log.last().volume == 0.5F);
      CHECK(log.last().muted == true);
      CHECK(log.last().iconName == "audio-volume-muted-symbolic");
      CHECK(log.last().tooltip == "Volume: 50% (Muted)");
    }

    SECTION("Mute controls")
    {
      viewModel.handleMutedChanged(true);
      CHECK(playback.state().muted == true);
      CHECK(log.last().muted == true);

      viewModel.toggleMuted();
      CHECK(playback.state().muted == false);
      CHECK(log.last().muted == false);
    }

    SECTION("handleScroll changes volume and unmutes if increasing")
    {
      viewModel.handleVolumeChanged(0.5F);
      viewModel.handleMutedChanged(true);

      viewModel.handleScroll(-1.0); // scroll up
      CHECK(playback.state().volume == Catch::Approx{0.52F}.margin(0.001F));
      CHECK(playback.state().muted == false); // Clears mute

      viewModel.handleScroll(1.0); // scroll down
      CHECK(playback.state().volume == Catch::Approx{0.50F}.margin(0.001F));
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

    SECTION("resolveIconName")
    {
      CHECK(VolumeViewModel::resolveIconName(0.5F, true) == "audio-volume-muted-symbolic");
      CHECK(VolumeViewModel::resolveIconName(0.0F, false) == "audio-volume-muted-symbolic");
      CHECK(VolumeViewModel::resolveIconName(0.25F, false) == "audio-volume-low-symbolic");
      CHECK(VolumeViewModel::resolveIconName(0.50F, false) == "audio-volume-medium-symbolic");
      CHECK(VolumeViewModel::resolveIconName(0.75F, false) == "audio-volume-high-symbolic");
      CHECK(VolumeViewModel::resolveIconName(1.00F, false) == "audio-volume-high-symbolic");
    }

    SECTION("resolveTooltip")
    {
      CHECK(VolumeViewModel::resolveTooltip(0.64F, false, false) == "Volume: 64%");
      CHECK(VolumeViewModel::resolveTooltip(0.64F, true, false) == "Volume: 64% (Muted)");
      CHECK(VolumeViewModel::resolveTooltip(0.64F, false, true) == "Volume: 64% (Hardware)");
    }
  }
} // namespace ao::uimodel::test
