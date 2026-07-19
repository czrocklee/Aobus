// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/OutputDeviceController.h"

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/rt/library/LibraryChanges.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <cstdint>
#include <memory>

namespace ao::tui::test
{
  namespace
  {
    // Composes Playback over the mock-provider transport fixture so the
    // controller observes real device-change state through a coherent snapshot
    // instead of the legacy service directly.
    struct ControllerPlayback final
    {
      explicit ControllerPlayback(rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>& fixture)
        : sources{fixture.libraryFixture.library(), changes}
        , views{fixture.executor, fixture.libraryFixture.library(), sources}
        , succession{fixture.executor,
                     views,
                     sources,
                     fixture.libraryFixture.library(),
                     fixture.playbackTransport,
                     fixture.notificationService,
                     fixture.asyncRuntime}
        , playbackBootstrap{fixture.playbackTransport}
        , playbackPtr{playbackBootstrap.createPlaybackService(fixture.executor, succession)}
        , playback{*playbackPtr}
      {
      }

      rt::LibraryChanges changes{};
      rt::TrackSourceCache sources;
      rt::ViewService views;
      rt::PlaybackSuccession succession;
      rt::PlaybackBootstrap playbackBootstrap;
      std::unique_ptr<rt::PlaybackService> playbackPtr;
      rt::PlaybackService& playback;
    };
  } // namespace

  TEST_CASE("OutputDeviceController - tracks selectable output rows", "[tui][unit][output]")
  {
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    fixture.status.descriptor.supportedProfiles.push_back(
      audio::BackendProvider::ProfileDescriptor{.id = audio::kProfileExclusive});
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto controllerPlayback = ControllerPlayback{fixture};
    std::int32_t refreshCount = 0;
    auto controller = OutputDeviceController{controllerPlayback.playback, [&refreshCount] { ++refreshCount; }};

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
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    fixture.status.descriptor.supportedProfiles.push_back(
      audio::BackendProvider::ProfileDescriptor{.id = audio::kProfileExclusive});
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto controllerPlayback = ControllerPlayback{fixture};
    auto controller = OutputDeviceController{controllerPlayback.playback};

    CHECK_FALSE(controller.selectRow(-1));
    CHECK_FALSE(controller.selectRow(0));
    CHECK(controller.selectedRow() == 1);
    CHECK(fixture.playbackTransport.state().output.selectedDevice.backendId == audio::BackendId{"mock_backend"});

    CHECK(controller.selectRow(1));
    auto const& selection = fixture.playbackTransport.state().output.selectedDevice;

    CHECK(selection.backendId == audio::BackendId{"mock_backend"});
    CHECK(selection.deviceId == audio::DeviceId{"mock_device"});
    CHECK(selection.profileId == audio::kProfileShared);

    REQUIRE(controller.viewState().rows.size() == 3);
    CHECK(controller.viewState().rows[2].profileId == audio::kProfileExclusive);
    CHECK(controller.selectRow(2));
    CHECK(controller.selectedRow() == 2);
  }
} // namespace ao::tui::test
