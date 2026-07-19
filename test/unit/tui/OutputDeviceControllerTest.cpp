// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/OutputDeviceController.h"

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/rt/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <cstdint>

namespace ao::tui::test
{
  TEST_CASE("OutputDeviceController - tracks selectable output rows", "[tui][unit][output]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::InlineExecutor>{};
    fixture.status.descriptor.supportedProfiles.push_back(
      audio::BackendProvider::ProfileDescriptor{.id = audio::kProfileExclusive});
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    std::int32_t refreshCount = 0;
    auto controller = OutputDeviceController{fixture.playbackService, [&refreshCount] { ++refreshCount; }};

    REQUIRE(refreshCount > 0);
    REQUIRE(controller.viewState().rows.size() == 3);
    CHECK(controller.viewState().outputBackendSummary == "mock_backend");
    CHECK(controller.selectedRow() == 1);

    CHECK(controller.moveSelection(1));
    CHECK(controller.selectedRow() == 2);
    CHECK(controller.moveSelection(-1));
    CHECK(controller.selectedRow() == 1);
    CHECK(controller.moveSelection(1));
    CHECK(controller.selectedRow() == 2);
    CHECK_FALSE(controller.moveSelection(1));
    CHECK(controller.selectedRow() == 2);
    CHECK(controller.moveSelection(-10));
    CHECK(controller.selectedRow() == 1);
    CHECK(controller.moveSelection(10));
    CHECK(controller.selectedRow() == 2);
    CHECK_FALSE(controller.moveSelection(0));
    CHECK(controller.selectedRow() == 2);
  }

  TEST_CASE("OutputDeviceController - selecting a row updates playback output", "[tui][unit][output]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::InlineExecutor>{};
    fixture.status.descriptor.supportedProfiles.push_back(
      audio::BackendProvider::ProfileDescriptor{.id = audio::kProfileExclusive});
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto controller = OutputDeviceController{fixture.playbackService};

    CHECK_FALSE(controller.selectRow(-1));
    CHECK_FALSE(controller.selectRow(0));
    CHECK(controller.selectedRow() == 1);
    CHECK(fixture.playbackService.state().output.selectedDevice.backendId == audio::BackendId{"mock_backend"});

    CHECK(controller.selectRow(1));
    auto const& selection = fixture.playbackService.state().output.selectedDevice;

    CHECK(selection.backendId == audio::BackendId{"mock_backend"});
    CHECK(selection.deviceId == audio::DeviceId{"mock_device"});
    CHECK(selection.profileId == audio::kProfileShared);

    REQUIRE(controller.viewState().rows.size() == 3);
    CHECK(controller.viewState().rows[2].profileId == audio::kProfileExclusive);
    CHECK(controller.selectRow(2));
    CHECK(controller.selectedRow() == 2);
  }
} // namespace ao::tui::test
