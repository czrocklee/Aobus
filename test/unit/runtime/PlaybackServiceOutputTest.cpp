// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/audio/Backend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <chrono>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("PlaybackService output device - devices output and quality signal subscriptions",
            "[runtime][unit][playback][output]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    // Prime the device list. The first notification auto-selects the default
    // output; the duplicate exercises the "already selected" early return, and the
    // empty list exercises the no-devices guard.
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto emptyStatus = fixture.status;
    emptyStatus.devices.clear();
    fixture.onDevicesChangedCb(emptyStatus.devices);

    bool devicesChangedFired = false;
    auto sub1 = fixture.playbackService.onOutputDevicesChanged([&] { devicesChangedFired = true; });

    bool outputChangedFired = false;
    auto lastOutputDevice = OutputDeviceSelection{};
    auto sub2 = fixture.playbackService.onOutputDeviceChanged(
      [&](auto const& ev)
      {
        outputChangedFired = true;
        lastOutputDevice = ev;
      });

    auto qualityEvents = std::vector<PlaybackService::QualityChanged>{};
    auto sub3 = fixture.playbackService.onQualityChanged([&](auto const& ev) { qualityEvents.push_back(ev); });

    fixture.playbackService.setOutputDevice(
      audio::BackendId{"mock_backend"}, audio::DeviceId{"mock_device"}, audio::ProfileId{audio::kProfileShared});
    CHECK(outputChangedFired);
    // setOutputDevice publishes the engine-confirmed selection taken from the
    // refreshed state, not the raw request, so the emitted event mirrors
    // state().selectedOutputDevice exactly (and stays consistent with the
    // auto-select path that also emits state.selectedOutputDevice).
    CHECK(lastOutputDevice.backendId == audio::BackendId{"mock_backend"});
    CHECK(lastOutputDevice.deviceId == audio::DeviceId{"mock_device"});
    CHECK(lastOutputDevice.profileId == audio::ProfileId{audio::kProfileShared});
    CHECK(lastOutputDevice == fixture.playbackService.state().selectedOutputDevice);
    CHECK(qualityEvents.empty());

    auto qualityFixture = PlaybackFixture<QueuedExecutor>{};
    auto routedQualityEvents = std::vector<PlaybackService::QualityChanged>{};
    auto qualitySub =
      qualityFixture.playbackService.onQualityChanged([&](auto const& ev) { routedQualityEvents.push_back(ev); });

    qualityFixture.onDevicesChangedCb(qualityFixture.status.devices);
    qualityFixture.executor.drain();
    CHECK(routedQualityEvents.empty());

    auto const testFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const desc =
      playbackRequest(TrackId{1}, testFile.string(), "Fake Track", "Fake Artist", std::chrono::minutes{2});
    CHECK(qualityFixture.playbackService.play(desc, ListId{1}));
    REQUIRE(qualityFixture.renderTarget != nullptr);

    qualityFixture.renderTarget->onRouteReady("mock_anchor");
    REQUIRE(qualityFixture.executor.drainUntil([&] { return !routedQualityEvents.empty(); }));

    REQUIRE(routedQualityEvents.size() == 1);
    CHECK(routedQualityEvents[0].quality == audio::Quality::BitwisePerfect);
    CHECK(routedQualityEvents[0].ready == true);
    CHECK(routedQualityEvents[0].quality == qualityFixture.playbackService.state().quality);
    CHECK(routedQualityEvents[0].ready == qualityFixture.playbackService.state().ready);
  }

  TEST_CASE("PlaybackService output device - device notification auto-configures output device before first play",
            "[runtime][unit][playback][output]")
  {
    // A harness receives its first device notification just before the play
    // request; the notification auto-selects the first available output device.
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const desc = playbackRequest(TrackId{1}, fixturePath, "Fake Track", "Fake Artist", std::chrono::minutes{2});

    CHECK(fixture.playbackService.play(desc, ListId{1}));
    CHECK(fixture.playbackService.state().trackId == TrackId{1});
  }

  TEST_CASE("PlaybackService output device - auto-select notifies device list subscribers",
            "[runtime][unit][playback][output]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    bool devicesChangedFired = false;
    auto sub = fixture.playbackService.onOutputDevicesChanged([&] { devicesChangedFired = true; });

    fixture.onDevicesChangedCb(fixture.status.devices);

    CHECK(devicesChangedFired);
    CHECK(fixture.playbackService.state().selectedOutputDevice.backendId == audio::BackendId{"mock_backend"});
    REQUIRE(fixture.playbackService.state().availableOutputBackends.size() == 1);
    REQUIRE(fixture.playbackService.state().availableOutputBackends.front().devices.size() == 1);
  }

  TEST_CASE("PlaybackService output device - auto-select skips unsupported default exclusive profile",
            "[runtime][unit][playback][output]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.status.devices = {
      audio::Device{.id = audio::DeviceId{},
                    .displayName = "System Default",
                    .description = "PipeWire",
                    .isDefault = true,
                    .backendId = audio::BackendId{"mock_backend"}},
    };
    fixture.status.metadata.supportedProfiles = {
      audio::IBackendProvider::ProfileMetadata{
        .id = audio::kProfileExclusive, .name = "Exclusive", .description = "Exclusive profile"},
      audio::IBackendProvider::ProfileMetadata{
        .id = audio::kProfileShared, .name = "Shared", .description = "Shared profile"},
    };
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);

    fixture.onDevicesChangedCb(fixture.status.devices);

    auto const& selection = fixture.playbackService.state().selectedOutputDevice;
    CHECK(selection.backendId == audio::BackendId{"mock_backend"});
    CHECK(selection.deviceId == audio::DeviceId{});
    CHECK(selection.profileId == audio::kProfileShared);
  }
} // namespace ao::rt::test
