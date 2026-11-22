// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include "runtime/playback/PlaybackTransport.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/ApplicationPlaybackTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/PlaybackOutputText.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("OutputDeviceViewModel - state generation")
  {
    auto fixture = ApplicationPlaybackFixture{};

    auto log = ao::test::RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{fixture.playback,
                                           ao::test::englishMessageCatalog(),
                                           [&log](auto const& view) { log.render(view); },
                                           OutputDeviceIntent::discarded()};

    SECTION("Initial state is empty when no outputs registered")
    {
      viewModel.refresh();
      REQUIRE(!log.empty());
      CHECK(log.last().rows.empty());
    }

    SECTION("Waiting for system default shows a localized status without any listed outputs")
    {
      viewModel.selectOutputDevice(audio::kBackendPipeWire, audio::DeviceId{}, audio::kProfileShared);
      viewModel.setTextCatalog(ao::test::messageCatalog("zh-Hant"));

      auto const& view = log.last();
      REQUIRE(view.rows.size() == 1);
      CHECK_FALSE(view.hasActiveOutputDevice);
      CHECK(view.outputBackendSummary == "選取輸出");
      CHECK(view.outputDeviceStatus == "所選輸出無法使用：系統預設");
      CHECK(view.rows.front().kind == OutputDeviceRow::Kind::Status);
      CHECK(view.rows.front().title == view.outputDeviceStatus);
      CHECK_FALSE(view.rows.front().isActive);
      auto const& optPending = fixture.playback.snapshot().transport.output.optPendingOutputSelection;
      REQUIRE(optPending);
      CHECK_FALSE(viewModel.tryToggleOutputRoutingPolicy(*optPending));
    }
  }

  TEST_CASE("OutputDeviceViewModel - asks for reselection of legacy and malformed PipeWire identities")
  {
    auto const* const deviceId = GENERATE("42", "node:", "unqualified");
    CAPTURE(deviceId);
    auto const request = audio::OutputDeviceSelection{
      .backendId = audio::kBackendPipeWire,
      .deviceId = audio::DeviceId{deviceId},
      .profileId = audio::kProfileShared,
    };
    auto fixture = ApplicationPlaybackFixture{};
    auto log = ao::test::RenderLog<OutputDeviceViewState>{};
    auto recorded = std::vector<audio::OutputDeviceSelection>{};
    auto viewModel = OutputDeviceViewModel{
      fixture.playback,
      ao::test::englishMessageCatalog(),
      [&log](auto const& view) { log.render(view); },
      OutputDeviceIntent::recordedBy([&recorded](auto const& selection) { recorded.push_back(selection); }),
    };
    fixture.commands().setOutputDevice(request);

    REQUIRE(fixture.playback.snapshot().transport.output.optPendingOutputSelection == request);
    REQUIRE_FALSE(log.empty());
    auto const& view = log.last();
    CHECK_FALSE(view.hasActiveOutputDevice);
    CHECK(view.outputDeviceStatus == "Select the PipeWire output again. The saved choice is invalid.");
    REQUIRE(view.rows.size() == 1);
    CHECK(view.rows.front().kind == OutputDeviceRow::Kind::Status);
    CHECK(view.rows.front().title == view.outputDeviceStatus);

    CHECK_FALSE(viewModel.tryToggleOutputRoutingPolicy(request));
    CHECK(fixture.playback.snapshot().transport.output.optPendingOutputSelection == request);
    CHECK(recorded.empty());
  }

  TEST_CASE("OutputDeviceViewModel - a qualified numeric PipeWire name can wait for discovery")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto log = ao::test::RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{fixture.playback,
                                           ao::test::englishMessageCatalog(),
                                           [&log](auto const& view) { log.render(view); },
                                           OutputDeviceIntent::discarded()};
    auto const request = audio::OutputDeviceSelection{
      .backendId = audio::kBackendPipeWire,
      .deviceId = audio::DeviceId{"node:42"},
      .profileId = audio::kProfileShared,
    };
    fixture.commands().setOutputDevice(request);

    REQUIRE_FALSE(log.empty());
    auto const& pending = log.last();
    CHECK_FALSE(pending.hasActiveOutputDevice);
    CHECK(pending.outputDeviceStatus == "Requested output unavailable");
    REQUIRE(pending.rows.size() == 2);
    CHECK(pending.rows[0].kind == OutputDeviceRow::Kind::Status);
    CHECK(pending.rows[1].kind == OutputDeviceRow::Kind::RoutingPolicy);
    CHECK(pending.rows[1].optRoutingPolicySelection == request);

    auto status = makePipeWireOutputStatus();
    status.devices.front().id = request.deviceId;
    status.devices.front().displayName = "USB DAC";
    addReadyAudioProvider(fixture.playbackTransport, std::move(status));

    CHECK(log.last().hasActiveOutputDevice);
    CHECK(log.last().outputDeviceStatus == "PipeWire: USB DAC");
  }

  TEST_CASE("OutputDeviceViewModel - toggles routing consent for the current pending PipeWire device")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto rendered = OutputDeviceViewState{};
    auto recorded = std::vector<audio::OutputDeviceSelection>{};
    auto viewModel = OutputDeviceViewModel{
      fixture.playback,
      ao::test::messageCatalog("zh-Hant"),
      [&rendered](OutputDeviceViewState const& view)
      {
        rendered = {};
        rendered = view;
      },
      OutputDeviceIntent::recordedBy([&recorded](auto const& selection) { recorded.push_back(selection); }),
    };
    auto const dacId = audio::DeviceId{"node:" + std::string(160, 'x')};

    viewModel.selectOutputDevice(audio::kBackendPipeWire, dacId, audio::kProfileShared);

    REQUIRE(rendered.rows.size() == 2);
    CHECK(rendered.rows[0].kind == OutputDeviceRow::Kind::Status);
    CHECK(rendered.rows[0].title == "所選輸出無法使用");
    CHECK(rendered.rows[1].kind == OutputDeviceRow::Kind::RoutingPolicy);
    CHECK(rendered.rows[1].title == "允許系統使用其他輸出");
    CHECK(rendered.rows[1].description ==
          "開啟後，如果此裝置在啟動時無法使用或中斷連線，系統可以使用其他輸出。關閉時僅使用此裝置。");
    CHECK_FALSE(rendered.rows[1].isActive);
    REQUIRE(rendered.rows[1].optRoutingPolicySelection);
    auto const fixedRequest = *rendered.rows[1].optRoutingPolicySelection;
    CHECK(fixedRequest.routingPolicy == audio::OutputRoutingPolicy::Fixed);
    CHECK(fixedRequest.deviceId == dacId);

    // Borrowed row identity must not survive the synchronous render during toggle.
    REQUIRE(viewModel.tryToggleOutputRoutingPolicy(*rendered.rows[1].optRoutingPolicySelection));

    REQUIRE(rendered.rows.size() == 2);
    CHECK(rendered.rows[0].kind == OutputDeviceRow::Kind::Status);
    CHECK_FALSE(rendered.hasActiveOutputDevice);
    CHECK(rendered.rows[1].isActive);
    CHECK(rendered.rows[1].description == "如果此裝置在啟動時無法使用或中斷連線，系統可以使用其他輸出。");
    REQUIRE(rendered.rows[1].optRoutingPolicySelection);
    auto const allowedRequest = *rendered.rows[1].optRoutingPolicySelection;
    CHECK(allowedRequest.deviceId == dacId);
    CHECK(allowedRequest.routingPolicy == audio::OutputRoutingPolicy::AllowSystemReroute);
    CHECK(fixture.playback.snapshot().transport.output.optPendingOutputSelection == allowedRequest);
    CHECK_FALSE(viewModel.tryToggleOutputRoutingPolicy(fixedRequest));
    REQUIRE(recorded.size() == 2);
    CHECK(recorded.front() == fixedRequest);
    CHECK(recorded.back() == allowedRequest);

    SECTION("Turning consent off restores strict routing for the same missing device")
    {
      REQUIRE(viewModel.tryToggleOutputRoutingPolicy(allowedRequest));

      REQUIRE(rendered.rows.size() == 2);
      CHECK_FALSE(rendered.rows[1].isActive);
      CHECK(rendered.rows[1].optRoutingPolicySelection == fixedRequest);
      CHECK(fixture.playback.snapshot().transport.output.optPendingOutputSelection == fixedRequest);
      CHECK(recorded.back() == fixedRequest);
    }

    SECTION("Fallback can be configured without exposing the opaque preferred-device ID")
    {
      addReadyAudioProvider(fixture.playbackTransport, makePipeWireOutputStatus());

      auto const& output = fixture.playback.snapshot().transport.output;
      CHECK_FALSE(output.optPendingOutputSelection);
      CHECK(output.selectedDevice == allowedRequest);
      CHECK(rendered.outputDeviceStatus == "系統可能使用其他輸出");
      REQUIRE(rendered.rows.size() == 4);
      CHECK(rendered.rows[0].kind == OutputDeviceRow::Kind::RoutingPolicy);
      CHECK_FALSE(rendered.rows[2].isActive);
      CHECK_FALSE(rendered.rows[3].isActive);

      REQUIRE(viewModel.tryToggleOutputRoutingPolicy(allowedRequest));

      CHECK(fixture.playback.snapshot().transport.output.optPendingOutputSelection == fixedRequest);
      CHECK_FALSE(rendered.hasActiveOutputDevice);
      CHECK(rendered.outputDeviceStatus == "所選輸出無法使用");
      REQUIRE(rendered.rows.size() == 5);
      CHECK(rendered.rows[0].kind == OutputDeviceRow::Kind::Status);
      CHECK(rendered.rows[1].kind == OutputDeviceRow::Kind::RoutingPolicy);
      CHECK_FALSE(rendered.rows[1].isActive);
    }

    SECTION("Discovery keeps consent and names the preferred device but does not identify the physical sink")
    {
      auto status = makePipeWireOutputStatus();
      status.devices.front().id = dacId;
      status.devices.front().displayName = "USB DAC";
      auto const receiverId = audio::DeviceId{"node:receiver"};
      status.devices.push_back(
        audio::Device{.id = receiverId, .displayName = "Receiver", .backendId = audio::kBackendPipeWire});
      addReadyAudioProvider(fixture.playbackTransport, std::move(status));

      auto const& recovered = fixture.playback.snapshot().transport.output;
      CHECK_FALSE(recovered.optPendingOutputSelection);
      CHECK(recovered.selectedDevice == allowedRequest);
      CHECK_FALSE(selectedOutputDeviceName(ao::test::englishMessageCatalog(), recovered));
      REQUIRE_FALSE(rendered.rows.empty());
      CHECK(rendered.rows[0].kind == OutputDeviceRow::Kind::RoutingPolicy);
      CHECK(rendered.rows[0].isActive);
      CHECK(rendered.outputDeviceStatus == "偏好輸出（系統可能切換）：PipeWire: USB DAC");

      viewModel.selectOutputDevice(audio::kBackendPipeWire, dacId, audio::kProfileExclusive);

      CHECK(recorded.back().profileId == audio::kProfileExclusive);
      CHECK(recorded.back().routingPolicy == audio::OutputRoutingPolicy::AllowSystemReroute);
      CHECK(fixture.playback.snapshot().transport.output.selectedDevice == recorded.back());
      auto const oldRoute = recorded.back();

      viewModel.selectOutputDevice(audio::kBackendPipeWire, receiverId, audio::kProfileShared);

      auto const& changed = fixture.playback.snapshot().transport.output;
      CHECK(changed.selectedDevice == recorded.back());
      CHECK(changed.selectedDevice.deviceId == receiverId);
      CHECK(changed.selectedDevice.routingPolicy == audio::OutputRoutingPolicy::Fixed);
      CHECK(selectedOutputDeviceName(ao::test::englishMessageCatalog(), changed) == "Receiver");
      REQUIRE_FALSE(rendered.rows.empty());
      CHECK_FALSE(rendered.rows[0].isActive);
      auto const submissionsBeforeStaleToggle = recorded.size();
      CHECK_FALSE(viewModel.tryToggleOutputRoutingPolicy(oldRoute));
      CHECK(recorded.size() == submissionsBeforeStaleToggle);
    }
  }

  TEST_CASE("OutputDeviceViewModel - explains exclusive access before and after rerouting consent")
  {
    auto fixture = ApplicationPlaybackFixture{};
    addReadyAudioProvider(fixture.playbackTransport, makePipeWireOutputStatus());
    auto log = ao::test::RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{fixture.playback,
                                           ao::test::englishMessageCatalog(),
                                           [&log](auto const& view) { log.render(view); },
                                           OutputDeviceIntent::discarded()};

    viewModel.selectOutputDevice(audio::kBackendPipeWire, audio::DeviceId{"node:device1"}, audio::kProfileExclusive);

    auto const explanation = std::string{
      "On: the system may open another output, still in Exclusive Mode, if this device is missing at startup or "
      "disconnects. Playback may fail if the output is busy. While Aobus holds it, other apps may be unable to use "
      "it."};
    REQUIRE_FALSE(log.empty());
    REQUIRE_FALSE(log.last().rows.empty());
    auto const& fixed = log.last().rows.front();
    REQUIRE(fixed.kind == OutputDeviceRow::Kind::RoutingPolicy);
    REQUIRE(fixed.optRoutingPolicySelection);
    CHECK_FALSE(fixed.isActive);
    CHECK(fixed.description == "Off: use only this device. " + explanation);

    REQUIRE(viewModel.tryToggleOutputRoutingPolicy(*fixed.optRoutingPolicySelection));

    REQUIRE_FALSE(log.last().rows.empty());
    auto const& allowed = log.last().rows.front();
    REQUIRE(allowed.kind == OutputDeviceRow::Kind::RoutingPolicy);
    CHECK(allowed.isActive);
    CHECK(allowed.description == explanation);
  }

  TEST_CASE("OutputDeviceViewModel - pending DAC clears the selected row until an output is confirmed")
  {
    auto fixture = ApplicationPlaybackFixture{};
    addReadyAudioProvider(fixture.playbackTransport, makePipeWireOutputStatus());
    auto log = ao::test::RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{fixture.playback,
                                           ao::test::englishMessageCatalog(),
                                           [&log](auto const& view) { log.render(view); },
                                           OutputDeviceIntent::discarded()};
    viewModel.refresh();
    REQUIRE(log.last().rows.size() == 4);
    REQUIRE(log.last().rows[2].isActive);
    auto const dacId = audio::DeviceId{std::string(160, 'x')};

    viewModel.selectOutputDevice(audio::kBackendAlsa, dacId, audio::kProfileExclusive);

    auto const& pending = log.last();
    REQUIRE(pending.rows.size() == 4);
    CHECK_FALSE(pending.hasActiveOutputDevice);
    CHECK(pending.outputBackendSummary == "Choose output");
    CHECK(pending.outputDeviceStatus == "Requested output unavailable");
    CHECK(pending.rows[0].kind == OutputDeviceRow::Kind::Status);
    CHECK(pending.rows[0].title == pending.outputDeviceStatus);
    CHECK_FALSE(pending.rows[0].isActive);
    CHECK(pending.rows[1].kind == OutputDeviceRow::Kind::BackendHeader);
    CHECK(pending.rows[2].kind == OutputDeviceRow::Kind::DeviceProfile);
    CHECK(pending.rows[2].title == "Built-in Audio");
    CHECK_FALSE(pending.rows[2].isActive);
    CHECK_FALSE(pending.rows[3].isActive);

    SECTION("Selecting the previously configured route manually clears the pending status")
    {
      auto const& alternative = pending.rows[2];
      viewModel.selectOutputDevice(alternative.backendId, alternative.deviceId, alternative.profileId);

      auto const& view = log.last();
      REQUIRE(view.rows.size() == 4);
      CHECK(view.rows[0].kind == OutputDeviceRow::Kind::RoutingPolicy);
      CHECK_FALSE(view.rows[0].isActive);
      CHECK(view.rows[1].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK(view.rows[2].isActive);
      CHECK(view.hasActiveOutputDevice);
      CHECK(view.outputBackendSummary == "PW");
      CHECK(view.outputDeviceStatus == "PipeWire: Built-in Audio");
    }

    SECTION("The discovered DAC becomes the active row and the advisory disappears")
    {
      auto status = makePipeWireOutputStatus();
      status.descriptor.id = audio::kBackendAlsa;
      status.devices.front().backendId = audio::kBackendAlsa;
      status.devices.front().id = dacId;
      status.devices.front().displayName = "USB DAC";
      addReadyAudioProvider(fixture.playbackTransport, std::move(status));

      auto const& view = log.last();
      REQUIRE(view.rows.size() == 6);
      CHECK(view.rows[0].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK_FALSE(view.rows[1].isActive);
      CHECK(view.rows[5].isActive);
      CHECK(view.rows[5].title == "USB DAC");
      CHECK(view.hasActiveOutputDevice);
      CHECK(view.outputBackendSummary == "ALSA");
      CHECK(view.outputDeviceStatus == "ALSA: USB DAC (Exclusive Mode)");
    }
  }

  TEST_CASE("OutputDeviceViewModel - refresh with fake provider")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;

    auto log = ao::test::RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{playback,
                                           ao::test::englishMessageCatalog(),
                                           [&log](auto const& view) { log.render(view); },
                                           OutputDeviceIntent::discarded()};

    SECTION("refresh shows backend header and device×profile rows")
    {
      addReadyAudioProvider(playbackTransport, makePipeWireOutputStatus());
      viewModel.refresh();

      REQUIRE(!log.empty());
      auto const& rows = log.last().rows;
      REQUIRE(rows.size() == 4); // 1 toggle + 1 header + 1 device × 2 profiles

      CHECK(rows[0].kind == OutputDeviceRow::Kind::RoutingPolicy);
      CHECK_FALSE(rows[0].isActive);
      CHECK(rows[1].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK(rows[1].backendId == audio::BackendId{"pipewire"});
      CHECK(rows[1].title == "PipeWire");

      for (std::size_t i = 2; i < rows.size(); ++i)
      {
        CHECK(rows[i].kind == OutputDeviceRow::Kind::DeviceProfile);
        CHECK(rows[i].deviceId == audio::DeviceId{"node:device1"});
        CHECK(rows[i].backendId == audio::BackendId{"pipewire"});
      }

      CHECK(rows[2].profileId == audio::kProfileShared);
      CHECK(rows[2].title == "Built-in Audio");
      CHECK(rows[2].description == "Built-in analog stereo");
      CHECK(rows[2].isExclusive == false);
      CHECK(rows[3].profileId == audio::kProfileExclusive);
      CHECK(rows[3].title == "Built-in Audio");
      CHECK(rows[3].description == "Built-in analog stereo");
      CHECK(rows[3].isExclusive == true);
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

      addReadyAudioProvider(playbackTransport, std::move(status));
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
      addReadyAudioProvider(playbackTransport, makePipeWireOutputStatus());
      playbackTransport.setOutputDevice(
        audio::BackendId{"pipewire"}, audio::DeviceId{"node:device1"}, audio::kProfileExclusive);
      viewModel.refresh();

      REQUIRE(!log.empty());
      auto const& rows = log.last().rows;
      REQUIRE(rows.size() == 4);
      CHECK(rows[3].isActive);
      CHECK(rows[3].profileId == audio::kProfileExclusive);
      CHECK_FALSE(rows[2].isActive);
    }

    SECTION("selectOutputDevice triggers playback state change")
    {
      addReadyAudioProvider(playbackTransport, makePipeWireOutputStatus());
      viewModel.selectOutputDevice(
        audio::BackendId{"pipewire"}, audio::DeviceId{"node:device1"}, audio::kProfileShared);

      auto const& sel = playbackTransport.state().output.selectedDevice;
      CHECK(sel.backendId == audio::BackendId{"pipewire"});
      CHECK(sel.deviceId == audio::DeviceId{"node:device1"});
      CHECK(sel.profileId == audio::kProfileShared);
    }

    SECTION("multiple backends produce separate header rows")
    {
      auto status2 = makePipeWireOutputStatus();
      status2.descriptor.id = audio::BackendId{"alsa"};
      status2.devices[0].backendId = audio::BackendId{"alsa"};
      status2.devices[0].id = audio::DeviceId{"alsa-device1"};

      addReadyAudioProvider(playbackTransport, makePipeWireOutputStatus());
      addReadyAudioProvider(playbackTransport, std::move(status2));
      viewModel.refresh();

      auto const& rows = log.last().rows;
      REQUIRE(rows.size() == 7);
      CHECK(rows[0].kind == OutputDeviceRow::Kind::RoutingPolicy);
      CHECK(rows[1].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK(rows[1].title == "PipeWire");
      CHECK(rows[4].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK(rows[4].title == "ALSA");
    }

    SECTION("summary fields for PipeWire shared output")
    {
      addReadyAudioProvider(playbackTransport, makePipeWireOutputStatus());
      playbackTransport.setOutputDevice(
        audio::BackendId{"pipewire"}, audio::DeviceId{"node:device1"}, audio::kProfileShared);
      viewModel.refresh();

      auto const& view = log.last();
      CHECK(view.hasActiveOutputDevice == true);
      CHECK(view.outputBackendSummary == "PW");
      CHECK(view.outputDeviceStatus == "PipeWire: Built-in Audio");
    }

    SECTION("summary fields for ALSA exclusive output")
    {
      auto status = makePipeWireOutputStatus();
      status.descriptor.id = audio::BackendId{"alsa"};
      status.devices[0].backendId = audio::BackendId{"alsa"};
      status.devices[0].id = audio::DeviceId{"device1"};
      status.devices[0].displayName = "USB DAC";

      addReadyAudioProvider(playbackTransport, std::move(status));
      playbackTransport.setOutputDevice(audio::BackendId{"alsa"}, audio::DeviceId{"device1"}, audio::kProfileExclusive);
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

  TEST_CASE("OutputDeviceViewModel - unrelated playback snapshots do not rebuild output rows")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto log = ao::test::RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{fixture.playback,
                                           ao::test::englishMessageCatalog(),
                                           [&log](auto const& view) { log.render(view); },
                                           OutputDeviceIntent::discarded()};
    viewModel.refresh();
    REQUIRE(log.states.size() == 1);
    fixture.commands().setShuffleMode(ShuffleMode::On);
    fixture.commands().setRepeatMode(RepeatMode::All);

    REQUIRE(fixture.playback.snapshot().succession.shuffle == ShuffleMode::On);
    REQUIRE(fixture.playback.snapshot().succession.repeat == RepeatMode::All);
    CHECK(log.states.size() == 1);
  }

  TEST_CASE("OutputDeviceViewModel - stopping observation keeps explicit refresh without resubscribing")
  {
    auto fixture = ApplicationPlaybackFixture{};
    addReadyAudioProvider(fixture.playbackTransport, makePipeWireOutputStatus());
    auto log = ao::test::RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{fixture.playback,
                                           ao::test::englishMessageCatalog(),
                                           [&log](auto const& view) { log.render(view); },
                                           OutputDeviceIntent::discarded()};

    viewModel.refresh();
    REQUIRE(log.states.size() == 1);
    REQUIRE(log.last().rows.size() == 4);
    REQUIRE(log.last().rows[2].isActive);

    fixture.playbackTransport.setOutputDevice(
      audio::BackendId{"pipewire"}, audio::DeviceId{"node:device1"}, audio::kProfileExclusive);
    REQUIRE(log.states.size() == 2);
    CHECK(log.last().rows[3].isActive);

    viewModel.stopObserving();
    viewModel.stopObserving();
    fixture.playbackTransport.setOutputDevice(
      audio::BackendId{"pipewire"}, audio::DeviceId{"node:device1"}, audio::kProfileShared);
    CHECK(fixture.playback.snapshot().transport.output.selectedDevice.profileId == audio::kProfileShared);
    REQUIRE(log.states.size() == 2);
    CHECK(log.last().rows[3].isActive);

    viewModel.refresh();
    REQUIRE(log.states.size() == 3);
    CHECK(log.last().outputDeviceStatus == "PipeWire: Built-in Audio");
    CHECK(log.last().rows[2].isActive);

    fixture.playbackTransport.setOutputDevice(
      audio::BackendId{"pipewire"}, audio::DeviceId{"node:device1"}, audio::kProfileExclusive);
    CHECK(fixture.playback.snapshot().transport.output.selectedDevice.profileId == audio::kProfileExclusive);
    REQUIRE(log.states.size() == 3);
    CHECK(log.last().rows[2].isActive);
  }

  TEST_CASE("OutputDeviceViewModel - reports exact requested intent when the engine cannot confirm it")
  {
    auto fixture = ApplicationPlaybackFixture{};
    addReadyAudioProvider(fixture.playbackTransport, makePipeWireOutputStatus());
    auto optRequested = std::optional<audio::OutputDeviceSelection>{};
    auto viewModel = OutputDeviceViewModel{
      fixture.playback,
      ao::test::englishMessageCatalog(),
      {},
      OutputDeviceIntent::recordedBy([&optRequested](audio::OutputDeviceSelection const& selection)
                                     { optRequested = selection; }),
    };

    viewModel.selectOutputDevice(
      audio::BackendId{"pipewire"}, audio::DeviceId{"node:temporarily-unavailable"}, audio::kProfileExclusive);

    REQUIRE(optRequested);
    CHECK(optRequested->backendId == audio::BackendId{"pipewire"});
    CHECK(optRequested->deviceId == audio::DeviceId{"node:temporarily-unavailable"});
    CHECK(optRequested->profileId == audio::kProfileExclusive);
    CHECK(fixture.playback.snapshot().transport.output.selectedDevice != *optRequested);
  }

  TEST_CASE("OutputDeviceViewModel - copies row identity before a synchronous render replaces the rows")
  {
    auto fixture = ApplicationPlaybackFixture{};
    addReadyAudioProvider(fixture.playbackTransport, makePipeWireOutputStatus());
    auto rendered = OutputDeviceViewState{};
    std::size_t renderCount = 0;
    auto optRequested = std::optional<audio::OutputDeviceSelection>{};
    auto viewModel = OutputDeviceViewModel{
      fixture.playback,
      ao::test::englishMessageCatalog(),
      [&rendered, &renderCount](OutputDeviceViewState const& view)
      {
        ++renderCount;
        auto replacement = view;
        rendered = {};
        rendered = std::move(replacement);
      },
      OutputDeviceIntent::recordedBy([&optRequested](audio::OutputDeviceSelection const& selection)
                                     { optRequested = selection; }),
    };
    viewModel.refresh();
    REQUIRE(rendered.rows.size() == 4);
    REQUIRE_FALSE(rendered.rows[3].isActive);
    auto const renderCountBeforeSelection = renderCount;
    auto const& row = rendered.rows[3];

    viewModel.selectOutputDevice(row.backendId, row.deviceId, row.profileId);

    REQUIRE(renderCount > renderCountBeforeSelection);
    REQUIRE(rendered.rows.size() == 4);
    CHECK(rendered.rows[3].isActive);
    REQUIRE(optRequested);
    CHECK(optRequested->backendId == audio::BackendId{"pipewire"});
    CHECK(optRequested->deviceId == audio::DeviceId{"node:device1"});
    CHECK(optRequested->profileId == audio::kProfileExclusive);
  }
} // namespace ao::uimodel::test
