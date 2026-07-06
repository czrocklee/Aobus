// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/audio/Backend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("OutputDeviceViewModel - state generation", "[uimodel][unit][playback][output]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};
    auto playback = PlaybackService{executor, viewService, testLib.library(), notificationService};

    auto log = ao::test::RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial state is empty when no outputs registered")
    {
      viewModel.refresh();
      REQUIRE(!log.empty());
      CHECK(log.last().rows.empty());
    }

    SECTION("selectOutputDevice without registered outputs keeps the view state empty")
    {
      viewModel.selectOutputDevice(audio::BackendId{"t"}, audio::DeviceId{"d"}, audio::kProfileShared);
      viewModel.refresh();

      REQUIRE(!log.empty());
      CHECK(log.last().rows.empty());
    }
  }

  TEST_CASE("OutputDeviceViewModel - refresh with fake provider", "[uimodel][unit][playback][output]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};
    auto playback = PlaybackService{executor, viewService, testLib.library(), notificationService};

    auto log = ao::test::RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("refresh shows backend header and device×profile rows")
    {
      addReadyAudioProvider(playback, makePipeWireOutputStatus());
      viewModel.refresh();

      REQUIRE(!log.empty());
      auto const& rows = log.last().rows;
      REQUIRE(rows.size() == 3); // 1 header + 1 device × 2 profiles

      // Row 0: BackendHeader
      CHECK(rows[0].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK(rows[0].backendId == audio::BackendId{"pipewire"});
      CHECK(rows[0].title == "PipeWire");

      // Rows 1-2: DeviceProfile (Shared + Exclusive)
      for (std::size_t i = 1; i < rows.size(); ++i)
      {
        CHECK(rows[i].kind == OutputDeviceRow::Kind::DeviceProfile);
        CHECK(rows[i].deviceId == audio::DeviceId{"device1"});
        CHECK(rows[i].backendId == audio::BackendId{"pipewire"});
      }

      CHECK(rows[1].profileId == audio::kProfileShared);
      CHECK(rows[1].title == "Built-in Audio");
      CHECK(rows[1].description == "Built-in analog stereo");
      CHECK(rows[1].isExclusive == false);
      CHECK(rows[2].profileId == audio::kProfileExclusive);
      CHECK(rows[2].title == "Built-in Audio");
      CHECK(rows[2].description == "Built-in analog stereo");
      CHECK(rows[2].isExclusive == true);
    }

    SECTION("system default output is not offered as an exclusive target")
    {
      auto status = makePipeWireOutputStatus();
      status.devices = {
        audio::Device{.id = audio::DeviceId{},
                      .displayName = "System Default",
                      .description = "PipeWire",
                      .isDefault = true,
                      .backendId = audio::BackendId{"pipewire"}},
      };

      addReadyAudioProvider(playback, std::move(status));
      viewModel.refresh();

      REQUIRE(!log.empty());
      auto const& rows = log.last().rows;
      REQUIRE(rows.size() == 2);
      CHECK(rows[1].kind == OutputDeviceRow::Kind::DeviceProfile);
      CHECK(rows[1].title == "System Default");
      CHECK(rows[1].profileId == audio::kProfileShared);
      CHECK(rows[1].isExclusive == false);
    }

    SECTION("active device is highlighted after setOutputDevice")
    {
      addReadyAudioProvider(playback, makePipeWireOutputStatus());
      playback.setOutputDevice(audio::BackendId{"pipewire"}, audio::DeviceId{"device1"}, audio::kProfileExclusive);
      viewModel.refresh();

      auto const& rows = log.last().rows;
      // The Exclusive profile row should be active
      CHECK(rows[2].isActive == true);
      CHECK(rows[2].profileId == audio::kProfileExclusive);
      // The Shared profile row should NOT be active
      CHECK(rows[1].isActive == false);
    }

    SECTION("selectOutputDevice triggers playback state change")
    {
      addReadyAudioProvider(playback, makePipeWireOutputStatus());
      viewModel.selectOutputDevice(audio::BackendId{"pipewire"}, audio::DeviceId{"device1"}, audio::kProfileShared);

      auto const& sel = playback.state().output.selectedDevice;
      CHECK(sel.backendId == audio::BackendId{"pipewire"});
      CHECK(sel.deviceId == audio::DeviceId{"device1"});
      CHECK(sel.profileId == audio::kProfileShared);
    }

    SECTION("multiple backends produce separate header rows")
    {
      auto status2 = makePipeWireOutputStatus();
      status2.metadata.id = audio::BackendId{"alsa"};
      status2.metadata.name = "ALSA";
      status2.devices[0].backendId = audio::BackendId{"alsa"};
      status2.devices[0].id = audio::DeviceId{"alsa-device1"};

      addReadyAudioProvider(playback, makePipeWireOutputStatus());
      addReadyAudioProvider(playback, std::move(status2));
      viewModel.refresh();

      auto const& rows = log.last().rows;
      // Should have: PipeWire header + 2 PipeWire profiles + ALSA header + 2 ALSA profiles = 6 rows
      REQUIRE(rows.size() == 6);

      CHECK(rows[0].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK(rows[0].title == "PipeWire");
      CHECK(rows[3].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK(rows[3].title == "ALSA");
    }

    SECTION("summary fields for PipeWire shared output")
    {
      addReadyAudioProvider(playback, makePipeWireOutputStatus());
      playback.setOutputDevice(audio::BackendId{"pipewire"}, audio::DeviceId{"device1"}, audio::kProfileShared);
      viewModel.refresh();

      auto const& view = log.last();
      CHECK(view.hasActiveOutputDevice == true);
      CHECK(view.outputBackendSummary == "PW");
      CHECK(view.outputDeviceStatus == "PipeWire: Built-in Audio");
    }

    SECTION("summary fields for ALSA exclusive output")
    {
      auto status = makePipeWireOutputStatus();
      status.metadata.id = audio::BackendId{"alsa"};
      status.metadata.name = "ALSA";
      status.devices[0].backendId = audio::BackendId{"alsa"};
      status.devices[0].displayName = "USB DAC";

      addReadyAudioProvider(playback, std::move(status));
      playback.setOutputDevice(audio::BackendId{"alsa"}, audio::DeviceId{"device1"}, audio::kProfileExclusive);
      viewModel.refresh();

      auto const& view = log.last();
      CHECK(view.hasActiveOutputDevice == true);
      CHECK(view.outputBackendSummary == "ALSA");
      CHECK(view.outputDeviceStatus == "ALSA: USB DAC (Exclusive Mode)");
    }

    SECTION("summary fields when no output is selected")
    {
      // No provider added, so no output device can be selected
      viewModel.refresh();

      auto const& view = log.last();
      CHECK(view.hasActiveOutputDevice == false);
      CHECK(view.outputBackendSummary == "--");
      CHECK(view.outputDeviceStatus.empty());
    }
  }
} // namespace ao::uimodel::test
