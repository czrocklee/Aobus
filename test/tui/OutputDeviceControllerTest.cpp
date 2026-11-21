// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/OutputDeviceController.h"

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/audio/BackendTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/PlaybackTransportTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <cstddef>
#include <cstdint>
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
        , playback{playbackBootstrap.createPlaybackService(fixture.executor,
                                                           succession,
                                                           fixture.libraryFixture.library(),
                                                           changes)}
      {
      }

      rt::LibraryChanges changes;
      rt::TrackSourceCache sources;
      rt::ViewService views;
      rt::PlaybackSuccession succession;
      rt::PlaybackBootstrap playbackBootstrap;
      rt::PlaybackService playback;
    };
  } // namespace

  TEST_CASE("OutputDeviceController - tracks selectable output rows")
  {
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    fixture.status.descriptor.supportedProfiles.push_back(
      audio::BackendProvider::ProfileDescriptor{.id = audio::kProfileExclusive});
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto controllerPlayback = ControllerPlayback{fixture};
    std::int32_t refreshCount = 0;
    auto controller = OutputDeviceController{controllerPlayback.playback,
                                             ao::test::englishMessageCatalog(),
                                             uimodel::OutputDeviceIntent::discarded(),
                                             [&refreshCount] { ++refreshCount; }};

    REQUIRE(refreshCount > 0);
    REQUIRE(controller.viewState().rows.size() == 3);
    CHECK(controller.viewState().outputBackendSummary == "mock_backend");
    controller.prepareToOpen();
    CHECK(controller.selectedRow() == 1);

    CHECK(controller.tryMoveSelection(1));
    CHECK(controller.selectedRow() == 2);
    CHECK(controller.tryMoveSelection(-1));
    CHECK(controller.selectedRow() == 1);
    CHECK(controller.tryMoveSelection(1));
    CHECK(controller.selectedRow() == 2);
    CHECK_FALSE(controller.tryMoveSelection(1));
    CHECK(controller.selectedRow() == 2);
    CHECK(controller.tryMoveSelection(-10));
    CHECK(controller.selectedRow() == 1);
    CHECK(controller.tryMoveSelection(10));
    CHECK(controller.selectedRow() == 2);
    CHECK_FALSE(controller.tryMoveSelection(0));
    CHECK(controller.selectedRow() == 2);
  }

  TEST_CASE("OutputDeviceController - focuses the first device when the picker opens without selectable rows")
  {
    using RowKind = uimodel::OutputDeviceRow::Kind;
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    fixture.onDevicesChangedCb({});
    auto controllerPlayback = ControllerPlayback{fixture};
    auto controller = OutputDeviceController{
      controllerPlayback.playback, ao::test::englishMessageCatalog(), uimodel::OutputDeviceIntent::discarded()};

    controller.prepareToOpen();

    REQUIRE(controller.viewState().rows.size() == 1);
    CHECK(controller.viewState().rows.front().kind == RowKind::BackendHeader);
    CHECK(controller.selectedRow() == -1);

    fixture.onDevicesChangedCb(fixture.status.devices);

    REQUIRE(controller.viewState().rows.size() == 2);
    CHECK(controller.selectedRow() == 1);
    CHECK(controller.viewState().rows[1].kind == RowKind::DeviceProfile);
    CHECK(controller.viewState().rows[1].deviceId == audio::DeviceId{"mock_device"});
  }

  TEST_CASE("OutputDeviceController - keeps focus clear after the focused device disappears")
  {
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    auto const& primary = fixture.status.devices.front();
    auto const removable =
      audio::Device{.id = audio::DeviceId{"removable"}, .displayName = "Removable", .backendId = primary.backendId};
    auto const alternative =
      audio::Device{.id = audio::DeviceId{"alternative"}, .displayName = "Alternative", .backendId = primary.backendId};
    fixture.onDevicesChangedCb({primary, removable, alternative});
    auto controllerPlayback = ControllerPlayback{fixture};
    auto optRecorded = std::optional<audio::OutputDeviceSelection>{};
    auto controller = OutputDeviceController{
      controllerPlayback.playback,
      ao::test::englishMessageCatalog(),
      uimodel::OutputDeviceIntent::recordedBy([&optRecorded](auto const& selection) { optRecorded = selection; })};
    controller.prepareToOpen();
    REQUIRE(controller.tryMoveSelection(1));
    REQUIRE(controller.selectedRow() == 2);
    REQUIRE(controller.viewState().rows.size() == 4);
    REQUIRE(controller.viewState().rows[2].deviceId == removable.id);

    // A different device now occupies the same row index, but Enter must not select it.
    fixture.onDevicesChangedCb({primary, alternative});

    REQUIRE(controller.viewState().rows.size() == 3);
    CHECK(controller.viewState().rows[2].deviceId == alternative.id);
    CHECK(controller.selectedRow() == -1);
    CHECK(controller.activateSelected() == OutputDeviceActivation::None);

    controller.refresh();
    CHECK(controller.selectedRow() == -1);
    CHECK(controller.activateSelected() == OutputDeviceActivation::None);

    fixture.onDevicesChangedCb({alternative});
    REQUIRE(controller.viewState().rows.size() == 2);
    CHECK(controller.selectedRow() == -1);

    fixture.onDevicesChangedCb({});
    REQUIRE(controller.viewState().rows.size() == 1);
    fixture.onDevicesChangedCb({alternative});
    REQUIRE(controller.viewState().rows.size() == 2);
    CHECK(controller.selectedRow() == -1);
    CHECK(controller.activateSelected() == OutputDeviceActivation::None);
    CHECK_FALSE(optRecorded);

    SECTION("navigation restores focus")
    {
      CHECK(controller.tryMoveSelection(1));
    }

    SECTION("reopening restores focus")
    {
      controller.prepareToOpen();
    }

    REQUIRE(controller.selectedRow() == 1);
    CHECK(controller.activateSelected() == OutputDeviceActivation::DeviceSelected);
    REQUIRE(optRecorded);
    CHECK(optRecorded->deviceId == alternative.id);
  }

  TEST_CASE("OutputDeviceController - opening a named route focuses its confirmed profile rather than consent")
  {
    using RowKind = uimodel::OutputDeviceRow::Kind;
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    rt::test::addReadyAudioProvider(fixture.playbackTransport, rt::test::makePipeWireOutputStatus());
    auto controllerPlayback = ControllerPlayback{fixture};
    auto const named = audio::OutputDeviceSelection{.backendId = audio::kBackendPipeWire,
                                                    .deviceId = audio::DeviceId{"node:device1"},
                                                    .profileId = audio::kProfileExclusive};
    controllerPlayback.playback.commands().setOutputDevice(named);
    auto optRecorded = std::optional<audio::OutputDeviceSelection>{};
    auto controller = OutputDeviceController{
      controllerPlayback.playback,
      ao::test::englishMessageCatalog(),
      uimodel::OutputDeviceIntent::recordedBy([&optRecorded](auto const& selection) { optRecorded = selection; })};

    controller.prepareToOpen();

    auto const deviceRow = controller.selectedRow();
    REQUIRE(deviceRow > 0);
    auto const& focused = controller.viewState().rows.at(static_cast<std::size_t>(deviceRow));
    CHECK(focused.kind == RowKind::DeviceProfile);
    CHECK(focused.isActive);
    CHECK(focused.deviceId == named.deviceId);
    CHECK(focused.profileId == named.profileId);
    REQUIRE(controller.viewState().rows.front().kind == RowKind::RoutingPolicy);
    CHECK_FALSE(optRecorded);

    SECTION("immediate activation keeps the current route fixed")
    {
      CHECK(controller.activateSelected() == OutputDeviceActivation::DeviceSelected);
      CHECK(optRecorded == named);
    }

    SECTION("explicit navigation can toggle consent without losing focus")
    {
      REQUIRE(controller.tryMoveSelection(-deviceRow));
      REQUIRE(controller.selectedRow() == 0);
      CHECK(controller.activateSelected() == OutputDeviceActivation::RoutingPolicyChanged);
      auto allowed = named;
      allowed.routingPolicy = audio::OutputRoutingPolicy::AllowSystemReroute;
      CHECK(optRecorded == allowed);
      controller.refresh();
      CHECK(controller.selectedRow() == 0);
      CHECK(controller.viewState().rows.front().isActive);

      controller.prepareToOpen();

      REQUIRE(controller.selectedRow() == deviceRow);
      optRecorded.reset();
      CHECK(controller.activateSelected() == OutputDeviceActivation::DeviceSelected);
      CHECK(optRecorded == allowed);
    }
  }

  TEST_CASE("OutputDeviceController - pending consent alone does not take focus on opening")
  {
    using RowKind = uimodel::OutputDeviceRow::Kind;
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    fixture.onDevicesChangedCb({});
    auto controllerPlayback = ControllerPlayback{fixture};
    auto const missing = audio::OutputDeviceSelection{.backendId = audio::kBackendPipeWire,
                                                      .deviceId = audio::DeviceId{"node:device1"},
                                                      .profileId = audio::kProfileShared};
    controllerPlayback.playback.commands().setOutputDevice(missing);
    auto optRecorded = std::optional<audio::OutputDeviceSelection>{};
    auto controller = OutputDeviceController{
      controllerPlayback.playback,
      ao::test::englishMessageCatalog(),
      uimodel::OutputDeviceIntent::recordedBy([&optRecorded](auto const& selection) { optRecorded = selection; })};

    controller.prepareToOpen();

    REQUIRE(controller.viewState().rows.at(1).kind == RowKind::RoutingPolicy);
    CHECK(controller.selectedRow() == -1);
    CHECK(controller.activateSelected() == OutputDeviceActivation::None);
    CHECK_FALSE(optRecorded);

    SECTION("explicit activation reaches the checkbox and discovery respects that focus")
    {
      SECTION("keyboard navigation")
      {
        REQUIRE(controller.tryMoveSelection(1));
        REQUIRE(controller.selectedRow() == 1);
        CHECK(controller.activateSelected() == OutputDeviceActivation::RoutingPolicyChanged);
      }

      SECTION("direct row activation")
      {
        CHECK(controller.activateRow(1) == OutputDeviceActivation::RoutingPolicyChanged);
      }

      auto allowed = missing;
      allowed.routingPolicy = audio::OutputRoutingPolicy::AllowSystemReroute;
      CHECK(optRecorded == allowed);

      rt::test::addReadyAudioProvider(fixture.playbackTransport, rt::test::makePipeWireOutputStatus());

      CHECK(controller.selectedRow() == 0);
      CHECK(controller.viewState().rows.front().optRoutingPolicySelection == allowed);
    }

    SECTION("discovery of the requested route supplies the initial device focus")
    {
      fixture.onDevicesChangedCb(fixture.status.devices);
      CHECK(controller.selectedRow() == -1);
      CHECK(controller.activateSelected() == OutputDeviceActivation::None);

      rt::test::addReadyAudioProvider(fixture.playbackTransport, rt::test::makePipeWireOutputStatus());

      auto const selected = controller.selectedRow();
      REQUIRE(selected > 0);
      auto const& focused = controller.viewState().rows.at(static_cast<std::size_t>(selected));
      CHECK(focused.kind == RowKind::DeviceProfile);
      CHECK(focused.isActive);
      CHECK(focused.deviceId == missing.deviceId);
      CHECK_FALSE(optRecorded);
    }
  }

  TEST_CASE("OutputDeviceController - selecting a row records the requested route")
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
      ao::test::englishMessageCatalog(),
      uimodel::OutputDeviceIntent::recordedBy([&optRecorded](audio::OutputDeviceSelection const& selection)
                                              { optRecorded = selection; })};

    REQUIRE(controller.activateRow(1) == OutputDeviceActivation::DeviceSelected);

    REQUIRE(optRecorded);
    CHECK(optRecorded->backendId == audio::BackendId{"mock_backend"});
    CHECK(optRecorded->deviceId == audio::DeviceId{"mock_device"});
    CHECK(optRecorded->profileId == audio::kProfileShared);
  }

  TEST_CASE("OutputDeviceController - selecting a row updates playback output")
  {
    auto fixture = rt::test::PlaybackTransportFixture<rt::test::InlineExecutor>{};
    fakeit::When(Method(fixture.mockProvider, createBackend))
      .AlwaysDo(
        [&fixture](audio::Device const&, audio::ProfileId const& profile)
        {
          fakeit::When(Method(fixture.spyBackendPtr->mock(), profileId)).AlwaysReturn(profile);
          return fixture.spyBackendPtr->makeProxy();
        });
    fixture.status.descriptor.supportedProfiles.push_back(
      audio::BackendProvider::ProfileDescriptor{.id = audio::kProfileExclusive});
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto controllerPlayback = ControllerPlayback{fixture};
    auto controller = OutputDeviceController{
      controllerPlayback.playback, ao::test::englishMessageCatalog(), uimodel::OutputDeviceIntent::discarded()};

    CHECK(controller.activateRow(-1) == OutputDeviceActivation::None);
    CHECK(controller.activateRow(0) == OutputDeviceActivation::None);
    CHECK(controller.selectedRow() == 1);
    CHECK(fixture.playbackTransport.state().output.selectedDevice.backendId == audio::BackendId{"mock_backend"});

    CHECK(controller.activateRow(1) == OutputDeviceActivation::DeviceSelected);
    auto const& selection = fixture.playbackTransport.state().output.selectedDevice;

    CHECK(selection.backendId == audio::BackendId{"mock_backend"});
    CHECK(selection.deviceId == audio::DeviceId{"mock_device"});
    CHECK(selection.profileId == audio::kProfileShared);

    REQUIRE(controller.viewState().rows.size() == 3);
    CHECK(controller.viewState().rows[2].profileId == audio::kProfileExclusive);
    CHECK(controller.activateRow(2) == OutputDeviceActivation::DeviceSelected);
    CHECK(controller.selectedRow() == 2);
    auto const& exclusiveSelection = fixture.playbackTransport.state().output.selectedDevice;
    CHECK(exclusiveSelection.backendId == audio::BackendId{"mock_backend"});
    CHECK(exclusiveSelection.deviceId == audio::DeviceId{"mock_device"});
    CHECK(exclusiveSelection.profileId == audio::kProfileExclusive);
  }

  TEST_CASE("OutputDeviceController - presents Core Audio shared outputs")
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
      ao::test::englishMessageCatalog(),
      uimodel::OutputDeviceIntent::recordedBy([&optRecorded](audio::OutputDeviceSelection const& selection)
                                              { optRecorded = selection; })};
    REQUIRE(controller.activateRow(1) == OutputDeviceActivation::DeviceSelected);

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
