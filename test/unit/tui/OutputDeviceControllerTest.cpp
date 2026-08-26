// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/OutputDeviceController.h"

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/PresentationTextCatalogTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <cstdint>
#include <memory>
#include <optional>

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
        : changes{fixture.executor, 0, "test-library"}
        , sources{fixture.libraryFixture.library(), changes}
        , views{fixture.executor, fixture.libraryFixture.library(), sources, changes}
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

      rt::LibraryChanges changes;
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
    auto controller = OutputDeviceController{controllerPlayback.playback,
                                             ao::test::englishPresentationTextCatalog(),
                                             uimodel::OutputDeviceIntent::discarded(),
                                             [&refreshCount] { ++refreshCount; }};

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

  TEST_CASE("OutputDeviceController - selecting a row records the requested route", "[tui][unit][output]")
  {
    // TUI kept no output preference before it carried an intent, so a chosen
    // route survived only until the process exited.
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    fixture.status.descriptor.supportedProfiles.push_back(
      audio::BackendProvider::ProfileDescriptor{.id = audio::kProfileExclusive});
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto controllerPlayback = ControllerPlayback{fixture};
    auto optRecorded = std::optional<audio::OutputDeviceSelection>{};
    auto controller = OutputDeviceController{
      controllerPlayback.playback,
      ao::test::englishPresentationTextCatalog(),
      uimodel::OutputDeviceIntent::recordedBy([&optRecorded](audio::OutputDeviceSelection const& selection)
                                              { optRecorded = selection; })};

    REQUIRE(controller.selectRow(1));

    REQUIRE(optRecorded);
    CHECK(optRecorded->backendId == audio::BackendId{"mock_backend"});
    CHECK_FALSE(optRecorded->profileId.empty());
  }

  TEST_CASE("OutputDeviceController - selecting a row updates playback output", "[tui][unit][output]")
  {
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    fixture.status.descriptor.supportedProfiles.push_back(
      audio::BackendProvider::ProfileDescriptor{.id = audio::kProfileExclusive});
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto controllerPlayback = ControllerPlayback{fixture};
    auto controller = OutputDeviceController{controllerPlayback.playback,
                                             ao::test::englishPresentationTextCatalog(),
                                             uimodel::OutputDeviceIntent::discarded()};

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

  TEST_CASE("OutputDeviceController - presents Core Audio shared outputs", "[tui][unit][output][coreaudio]")
  {
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    fixture.status.descriptor.id = audio::kBackendCoreAudio;
    fixture.status.devices = {
      audio::Device{.id = audio::DeviceId{"coreaudio-device"},
                    .displayName = "Built-in Output",
                    .description = "Apple Inc.",
                    .isDefault = true,
                    .backendId = audio::kBackendCoreAudio},
    };
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto controllerPlayback = ControllerPlayback{fixture};
    auto optRecorded = std::optional<audio::OutputDeviceSelection>{};
    auto controller = OutputDeviceController{
      controllerPlayback.playback,
      ao::test::englishPresentationTextCatalog(),
      uimodel::OutputDeviceIntent::recordedBy([&optRecorded](audio::OutputDeviceSelection const& selection)
                                              { optRecorded = selection; })};
    REQUIRE(controller.selectRow(1));

    auto const& view = controller.viewState();
    REQUIRE(view.rows.size() == 2U);
    CHECK(view.rows[0].kind == uimodel::OutputDeviceRow::Kind::BackendHeader);
    CHECK(view.rows[0].title == "Core Audio");
    CHECK(view.rows[1].backendId == audio::kBackendCoreAudio);
    CHECK(view.rows[1].deviceId == audio::DeviceId{"coreaudio-device"});
    CHECK(view.rows[1].profileId == audio::kProfileShared);
    CHECK(view.rows[1].title == "Built-in Output");
    CHECK(view.rows[1].description == "Apple Inc.");
    REQUIRE(optRecorded);
    CHECK(optRecorded->backendId == audio::kBackendCoreAudio);
    CHECK(optRecorded->deviceId == audio::DeviceId{"coreaudio-device"});
    CHECK(optRecorded->profileId == audio::kProfileShared);
  }
} // namespace ao::tui::test
