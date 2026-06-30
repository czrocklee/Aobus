// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/OutputDeviceController.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/audio/Backend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/rt/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <cstdint>

namespace ao::tui::test
{
  TEST_CASE("OutputDeviceController - tracks selectable output rows", "[tui][unit][output]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::MockExecutor>{};
    fixture.status.metadata.supportedProfiles.push_back(audio::IBackendProvider::ProfileMetadata{
      .id = audio::kProfileExclusive, .name = "Exclusive", .description = "Exclusive profile"});
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    std::int32_t refreshCount = 0;
    auto controller = OutputDeviceController{fixture.playbackService, [&refreshCount] { ++refreshCount; }};

    REQUIRE(refreshCount > 0);
    REQUIRE(controller.viewState().rows.size() == 3);
    CHECK(controller.viewState().outputBackendSummary == "Mock Backend");
    CHECK(controller.selectedRow() == 1);

    CHECK(controller.moveSelection(1));
    CHECK(controller.selectedRow() == 2);
    CHECK(controller.moveSelection(-1));
    CHECK(controller.selectedRow() == 1);
    CHECK(controller.moveSelection(1));
    CHECK(controller.selectedRow() == 2);
    CHECK_FALSE(controller.moveSelection(1));
    CHECK(controller.selectedRow() == 2);
  }

  TEST_CASE("OutputDeviceController - selecting a row updates playback output", "[tui][unit][output]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::MockExecutor>{};
    fixture.status.metadata.supportedProfiles.push_back(audio::IBackendProvider::ProfileMetadata{
      .id = audio::kProfileExclusive, .name = "Exclusive", .description = "Exclusive profile"});
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto controller = OutputDeviceController{fixture.playbackService};

    CHECK(controller.selectRow(-1) == "No output device selected");
    CHECK(controller.selectRow(0) == "No output device selected");

    auto const status = controller.selectRow(1);
    auto const& selection = fixture.playbackService.state().selectedOutputDevice;

    CHECK(status == "Output: Mock Device");
    CHECK(selection.backendId == audio::BackendId{"mock_backend"});
    CHECK(selection.deviceId == audio::DeviceId{"mock_device"});
    CHECK(selection.profileId == audio::kProfileShared);

    CHECK(controller.selectRow(2) == "Output: Mock Device (Exclusive)");
  }
} // namespace ao::tui::test
