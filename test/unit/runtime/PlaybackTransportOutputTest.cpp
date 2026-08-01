// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Quality.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <chrono>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    audio::flow::Graph verifiedSystemGraph()
    {
      auto const format =
        audio::PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Signed16Le};
      return audio::flow::Graph{
        .nodes =
          {
            audio::flow::Node{.id = "system-stream",
                              .type = audio::flow::NodeType::Stream,
                              .name = "System Stream",
                              .optFormat = format},
            audio::flow::Node{
              .id = "system-sink", .type = audio::flow::NodeType::Sink, .name = "System Sink", .optFormat = format},
          },
        .connections =
          {
            audio::flow::Connection{.sourceId = "system-stream", .destinationId = "system-sink", .isActive = true},
          },
      };
    }
  } // namespace

  TEST_CASE("PlaybackTransport output device - devices output and quality signal subscriptions",
            "[runtime][unit][playback][output]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

    // Prime the device list. The first notification auto-selects the default
    // output; the duplicate exercises the "already selected" early return, and the
    // empty list exercises the no-devices guard.
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto emptyStatus = fixture.status;
    emptyStatus.devices.clear();
    fixture.onDevicesChangedCb(emptyStatus.devices);

    bool devicesChangedFired = false;
    auto sub1 = fixture.playbackTransport.onOutputDevicesChanged([&] noexcept { devicesChangedFired = true; });

    bool outputChangedFired = false;
    auto lastOutputDevice = OutputDeviceSelection{};
    auto sub2 = fixture.playbackTransport.onOutputDeviceChanged(
      [&](auto const& ev) noexcept
      {
        outputChangedFired = true;
        lastOutputDevice = ev;
      });

    auto qualityEvents = std::vector<PlaybackTransport::QualityChanged>{};
    auto sub3 =
      fixture.playbackTransport.onQualityChanged([&](auto const& ev) noexcept { qualityEvents.push_back(ev); });

    fixture.playbackTransport.setOutputDevice(
      audio::BackendId{"mock_backend"}, audio::DeviceId{"mock_device"}, audio::ProfileId{audio::kProfileShared});
    CHECK(outputChangedFired);
    // setOutputDevice publishes the engine-confirmed selection taken from the
    // refreshed state, not the raw request, so the emitted event mirrors
    // state().output.selectedDevice exactly (and stays consistent with the
    // auto-select path that also emits state.output.selectedDevice).
    CHECK(lastOutputDevice.backendId == audio::BackendId{"mock_backend"});
    CHECK(lastOutputDevice.deviceId == audio::DeviceId{"mock_device"});
    CHECK(lastOutputDevice.profileId == audio::ProfileId{audio::kProfileShared});
    CHECK(lastOutputDevice == fixture.playbackTransport.state().output.selectedDevice);
    CHECK(qualityEvents.empty());

    auto qualityFixture = PlaybackTransportFixture<QueuedExecutor>{};
    auto routedQualityEvents = std::vector<PlaybackTransport::QualityChanged>{};
    auto qualitySub = qualityFixture.playbackTransport.onQualityChanged([&](auto const& ev) noexcept
                                                                        { routedQualityEvents.push_back(ev); });

    qualityFixture.onDevicesChangedCb(qualityFixture.status.devices);
    qualityFixture.executor.drain();
    CHECK(routedQualityEvents.empty());

    auto const testFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const desc =
      playbackRequest(TrackId{1}, testFile.string(), "Fake Track", "Fake Artist", std::chrono::minutes{2});
    CHECK(qualityFixture.playbackTransport.play(desc, ListId{1}));
    REQUIRE(qualityFixture.renderTarget != nullptr);

    qualityFixture.renderTarget->handleRouteReady("mock_anchor");
    REQUIRE(qualityFixture.executor.drainUntil(
      [&] { return !routedQualityEvents.empty() && routedQualityEvents.back().ready; }));

    // Backends may publish intermediate graph updates while a route settles.
    // The service contract is the final ready payload, not an exact event count.
    auto const& qualityEvent = routedQualityEvents.back();
    CHECK(qualityEvent.quality.overall == audio::Quality::BitwisePerfect);
    CHECK(qualityEvent.quality.sourceQuality == qualityFixture.playbackTransport.state().quality.sourceQuality);
    CHECK(qualityEvent.quality.pipelineQuality == qualityFixture.playbackTransport.state().quality.pipelineQuality);
    CHECK(qualityEvent.quality.fullyVerified == qualityFixture.playbackTransport.state().quality.fullyVerified);
    CHECK(qualityEvent.ready == true);
    CHECK(qualityEvent.quality.overall == qualityFixture.playbackTransport.state().quality.overall);
    CHECK(qualityEvent.ready == qualityFixture.playbackTransport.state().ready);
  }

  TEST_CASE("PlaybackTransport output device - device notification auto-configures output device before first play",
            "[runtime][unit][playback][output]")
  {
    // A fixture receives its first device notification just before the play
    // request; the notification auto-selects the first available output device.
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const desc = playbackRequest(TrackId{1}, fixturePath, "Fake Track", "Fake Artist", std::chrono::minutes{2});

    CHECK(fixture.playbackTransport.play(desc, ListId{1}));
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == TrackId{1});
  }

  TEST_CASE("PlaybackTransport output device - paused playback continues to publish graph quality changes",
            "[runtime][regression][playback][output]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    auto qualityEvents = std::vector<PlaybackTransport::QualityChanged>{};
    auto qualitySub =
      fixture.playbackTransport.onQualityChanged([&](auto const& event) noexcept { qualityEvents.push_back(event); });

    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const testFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const request =
      playbackRequest(TrackId{1}, testFile.string(), "Paused Quality", "Aobus", std::chrono::minutes{2});
    REQUIRE(fixture.playbackTransport.play(request, ListId{1}));
    REQUIRE(fixture.renderTarget != nullptr);

    fixture.renderTarget->handleRouteReady("mock_anchor");
    REQUIRE(fixture.executor.drainUntil([&] { return static_cast<bool>(fixture.onGraphChangedCb); }));

    fixture.onGraphChangedCb(verifiedSystemGraph());
    REQUIRE(fixture.executor.drainUntil(
      [&]
      {
        return !qualityEvents.empty() &&
               qualityEvents.back().quality.pipelineQuality == audio::Quality::BitwisePerfect &&
               qualityEvents.back().quality.fullyVerified;
      }));

    fixture.playbackTransport.pause();
    REQUIRE(fixture.playbackTransport.state().transport == audio::Transport::Paused);

    auto interventionGraph = verifiedSystemGraph();
    interventionGraph.nodes.front().softwareVolumeNotUnity = true;
    interventionGraph.nodes.front().maxSoftwareGain = 0.5F;
    fixture.onGraphChangedCb(interventionGraph);
    REQUIRE(fixture.executor.drainUntil(
      [&] { return qualityEvents.back().quality.pipelineQuality == audio::Quality::LinearIntervention; }));

    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Paused);
    CHECK(qualityEvents.back().ready);
    CHECK(qualityEvents.back().quality.pipelineQuality == audio::Quality::LinearIntervention);
  }

  TEST_CASE("PlaybackTransport output device - auto-select notifies device list subscribers",
            "[runtime][unit][playback][output]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    bool devicesChangedFired = false;
    auto sub = fixture.playbackTransport.onOutputDevicesChanged([&] noexcept { devicesChangedFired = true; });

    fixture.onDevicesChangedCb(fixture.status.devices);

    CHECK(devicesChangedFired);
    CHECK(fixture.playbackTransport.state().output.selectedDevice.backendId == audio::BackendId{"mock_backend"});
    REQUIRE(fixture.playbackTransport.state().output.availableBackends.size() == 1);
    REQUIRE(fixture.playbackTransport.state().output.availableBackends.front().devices.size() == 1);
  }

  TEST_CASE("PlaybackTransport output device - auto-select skips unsupported default exclusive profile",
            "[runtime][unit][playback][output]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    fixture.status.devices = {
      audio::Device{.id = audio::DeviceId{},
                    .displayName = "System Default",
                    .description = "PipeWire",
                    .isDefault = true,
                    .backendId = audio::BackendId{"mock_backend"}},
    };
    fixture.status.descriptor.supportedProfiles = {
      audio::BackendProvider::ProfileDescriptor{.id = audio::kProfileExclusive},
      audio::BackendProvider::ProfileDescriptor{.id = audio::kProfileShared},
    };
    fakeit::When(Method(fixture.mockProvider, status)).AlwaysReturn(fixture.status);

    fixture.onDevicesChangedCb(fixture.status.devices);

    auto const& selection = fixture.playbackTransport.state().output.selectedDevice;
    CHECK(selection.backendId == audio::BackendId{"mock_backend"});
    CHECK(selection.deviceId == audio::DeviceId{});
    CHECK(selection.profileId == audio::kProfileShared);
  }
} // namespace ao::rt::test
