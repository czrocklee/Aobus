// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/audio/AudioFixtureSupport.h"
#include "test/audio/ScriptedDecoderSession.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/PlaybackTransportTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
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
#include <cstddef>
#include <memory>
#include <optional>
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

  TEST_CASE("PlaybackTransport output device - publishes confirmed selection without premature quality")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

    fixture.onDevicesChangedCb(fixture.status.devices);

    bool outputChangedFired = false;
    auto lastOutputDevice = audio::OutputDeviceSelection{};
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
  }

  TEST_CASE("PlaybackTransport output device - publishes confirmed quality after route readiness")
  {
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
    REQUIRE(qualityFixture.playbackTransport.play(desc, ListId{1}));
    REQUIRE(qualityFixture.renderTarget != nullptr);

    qualityFixture.renderTarget->handleRouteReady("mock_anchor");
    REQUIRE(qualityFixture.executor.tryDrainUntil(
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

  TEST_CASE("PlaybackTransport output device - publishes channel confidence independently of numerical quality")
  {
    auto origin = audio::ChannelLayoutOrigin::Unknown;
    auto expectedConfidence = audio::ChannelConfidence::Unknown;

    SECTION("identified scripted source with mapped delivery")
    {
      origin = audio::ChannelLayoutOrigin::Identified;
      expectedConfidence = audio::ChannelConfidence::Verified;
    }

    SECTION("inferred scripted source with mapped delivery")
    {
      origin = audio::ChannelLayoutOrigin::Inferred;
      expectedConfidence = audio::ChannelConfidence::Inferred;
    }

    auto const format = audio::PcmFormat{
      .sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Signed16Le, .speakerMask = 0x3};
    // Synthetic decoder facts exercise runtime propagation, not shipping decoder
    // provenance. Source history enters only through the existing decoder factory.
    auto const factory = [format, origin](auto const&, std::optional<audio::SampleEncoding> optOutputEncoding)
    {
      auto const sourceFormat = audio::signalFormat(format);
      auto decoderPtr = std::make_unique<audio::test::ScriptedDecoderSession>(audio::DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = audio::pcmFormat(sourceFormat, optOutputEncoding.value_or(format.encoding)),
        .duration = std::chrono::milliseconds{10},
        .isLossy = false,
        .sourceChannelLayout = {.origin = origin}});
      // PCM and EOF fit in preroll; no render loop or background decode wait is needed.
      decoderPtr->setReadScript({{.data = std::vector<std::byte>(1764, std::byte{0})}, {.endOfStream = true}});
      return decoderPtr;
    };
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{factory};
    auto qualityEvents = std::vector<PlaybackTransport::QualityChanged>{};
    auto qualitySub =
      fixture.playbackTransport.onQualityChanged([&](auto const& event) noexcept { qualityEvents.push_back(event); });

    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const request = playbackRequest(
      TrackId{1}, "scripted-channel-layout.flac", "Scripted Channel Layout", "Aobus", std::chrono::milliseconds{10});
    REQUIRE(fixture.playbackTransport.play(request, ListId{1}));
    REQUIRE(fixture.renderTarget != nullptr);

    fixture.renderTarget->handleRouteReady("mock_anchor");
    REQUIRE(fixture.executor.tryDrainUntil([&] { return static_cast<bool>(fixture.onGraphChangedCb); }));

    auto graph = verifiedSystemGraph();

    // The controlled provider independently reports FL/FR native delivery,
    // never source provenance or an ao-source.
    for (auto& node : graph.nodes)
    {
      node.channelDelivery = {.isMapped = true, .nativeSpeakerMask = 0x3};
    }

    fixture.onGraphChangedCb(graph);
    REQUIRE(fixture.executor.tryDrainUntil(
      [&]
      {
        return !qualityEvents.empty() && qualityEvents.back().ready &&
               qualityEvents.back().quality.channelConfidence == expectedConfidence;
      }));

    auto const mappedState = fixture.playbackTransport.state();
    auto const mappedEvent = qualityEvents.back();
    CHECK(mappedState.ready);
    CHECK(mappedState.quality.channelConfidence == expectedConfidence);
    CHECK(mappedState.quality.sourceQuality == audio::Quality::BitwisePerfect);
    CHECK(mappedState.quality.pipelineQuality == audio::Quality::BitwisePerfect);
    CHECK(mappedState.quality.overall == audio::Quality::BitwisePerfect);
    CHECK(mappedState.quality.fullyVerified);
    CHECK(mappedEvent.ready);
    CHECK(mappedEvent.quality.channelConfidence == expectedConfidence);
    CHECK(mappedEvent.quality == mappedState.quality);

    auto const eventCount = qualityEvents.size();
    // Withdraw only the sink's mapping proof, retaining its complete identity
    // and every numerical format. Confidence alone must reach state and observers.
    graph.nodes.back().channelDelivery.isMapped = false;
    fixture.onGraphChangedCb(graph);
    REQUIRE(fixture.executor.tryDrainUntil(
      [&]
      {
        return qualityEvents.size() > eventCount &&
               qualityEvents.back().quality.channelConfidence == audio::ChannelConfidence::Unverified;
      }));

    auto expectedQuality = mappedState.quality;
    expectedQuality.channelConfidence = audio::ChannelConfidence::Unverified;
    auto const& state = fixture.playbackTransport.state();
    auto const& event = qualityEvents.back();
    CHECK(state.quality.channelConfidence == audio::ChannelConfidence::Unverified);
    CHECK(state.quality == expectedQuality);
    CHECK(state.ready == mappedState.ready);
    CHECK(state.nowPlaying == mappedState.nowPlaying);
    CHECK(state.output == mappedState.output);
    CHECK(event.quality.channelConfidence == audio::ChannelConfidence::Unverified);
    CHECK(event.quality == expectedQuality);
    CHECK(event.ready == mappedEvent.ready);
  }

  TEST_CASE("PlaybackTransport output device - device notification auto-configures output device before first play")
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

  TEST_CASE("PlaybackTransport output device - paused playback continues to publish graph quality changes")
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
    REQUIRE(fixture.executor.tryDrainUntil([&] { return static_cast<bool>(fixture.onGraphChangedCb); }));

    fixture.onGraphChangedCb(verifiedSystemGraph());
    REQUIRE(fixture.executor.tryDrainUntil(
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
    REQUIRE(fixture.executor.tryDrainUntil(
      [&] { return qualityEvents.back().quality.pipelineQuality == audio::Quality::LinearIntervention; }));

    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Paused);
    CHECK(qualityEvents.back().ready);
    CHECK(qualityEvents.back().quality.pipelineQuality == audio::Quality::LinearIntervention);
  }

  TEST_CASE("PlaybackTransport output device - auto-select notifies device list subscribers")
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

  TEST_CASE("PlaybackTransport output device - repeated and empty device lists keep the confirmed selection")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const selected = fixture.playbackTransport.state().output.selectedDevice;
    REQUIRE(selected.backendId == audio::BackendId{"mock_backend"});
    std::size_t devicesChangedCount = 0;
    auto selections = std::vector<audio::OutputDeviceSelection>{};
    auto const devicesSub = fixture.playbackTransport.onOutputDevicesChanged([&] noexcept { ++devicesChangedCount; });
    auto const selectionSub = fixture.playbackTransport.onOutputDeviceChanged(
      [&](audio::OutputDeviceSelection const& selection) noexcept { selections.push_back(selection); });

    fixture.onDevicesChangedCb(fixture.status.devices);

    CHECK(devicesChangedCount == 1);
    CHECK(selections.empty());
    CHECK(fixture.playbackTransport.state().output.selectedDevice == selected);

    fixture.onDevicesChangedCb({});

    CHECK(devicesChangedCount == 2);
    CHECK(selections.empty());
    CHECK(fixture.playbackTransport.state().output.selectedDevice == selected);
    REQUIRE(fixture.playbackTransport.state().output.availableBackends.size() == 1);
    CHECK(fixture.playbackTransport.state().output.availableBackends.front().devices.empty());
  }

  TEST_CASE("PlaybackTransport output device - auto-select skips unsupported default exclusive profile")
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
