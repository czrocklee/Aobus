// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/audio/Backend.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/rt/StateTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("PlaybackService output - devices output and quality signal subscriptions",
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
    auto sub1 = fixture.playbackService.onDevicesChanged([&] { devicesChangedFired = true; });

    bool outputChangedFired = false;
    auto lastOutput = OutputSelection{};
    auto sub2 = fixture.playbackService.onOutputChanged(
      [&](auto const& ev)
      {
        outputChangedFired = true;
        lastOutput = ev;
      });

    auto qualityEvents = std::vector<PlaybackService::QualityChanged>{};
    auto sub3 = fixture.playbackService.onQualityChanged([&](auto const& ev) { qualityEvents.push_back(ev); });

    fixture.playbackService.setOutput(
      audio::BackendId{"mock_backend"}, audio::DeviceId{"mock_device"}, audio::ProfileId{audio::kProfileShared});
    CHECK(outputChangedFired);
    // setOutput publishes the engine-confirmed selection taken from the
    // refreshed state, not the raw request, so the emitted event mirrors
    // state().selectedOutput exactly (and stays consistent with the
    // auto-select path that also emits state.selectedOutput).
    CHECK(lastOutput.backendId == audio::BackendId{"mock_backend"});
    CHECK(lastOutput.deviceId == audio::DeviceId{"mock_device"});
    CHECK(lastOutput.profileId == audio::ProfileId{audio::kProfileShared});
    CHECK(lastOutput == fixture.playbackService.state().selectedOutput);
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

  TEST_CASE("PlaybackService output - device notification auto-configures output before first play",
            "[runtime][unit][playback][output]")
  {
    // A harness receives its first device notification just before the play
    // request; the notification auto-selects the first available output.
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);

    auto const desc =
      playbackRequest(TrackId{1}, "/fake/path.flac", "Fake Track", "Fake Artist", std::chrono::minutes{2});

    CHECK(fixture.playbackService.play(desc, ListId{1}));
    CHECK(fixture.playbackService.state().trackId == TrackId{1});
  }
} // namespace ao::rt::test
