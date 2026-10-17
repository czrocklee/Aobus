// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaGraphRegistry.h"

#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using namespace ao::audio::backend::detail;
using namespace ao::audio::flow;

TEST_CASE("AlsaGraphRegistry - delivery-only changes publish without relabelling native slots", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto graph = Graph{};
  std::size_t calls = 0;
  auto sub = registry.subscribe("hw:0,0",
                                [&](Graph const& nextGraph)
                                {
                                  graph = nextGraph;
                                  ++calls;
                                });
  auto mode = ao::audio::OpenedPcmMode{
    .clientFormat = {.sampleRate = 48000,
                     .channels = 4,
                     .encoding = ao::audio::SampleEncoding::Signed32Le,
                     .speakerMask = 0x33},
    .optEndpoint =
      ao::audio::ConfirmedEndpoint{.signalFormat = {.sampleRate = 48000, .channels = 4, .precisionBits = 24}},
    .channelDelivery = {.isMapped = false}};

  registry.publish({.routeAnchor = "hw:0,0", .optMode = mode});

  REQUIRE(calls == 2U);
  REQUIRE(graph.nodes.size() == 2U);
  REQUIRE(graph.nodes[0].optFormat);
  REQUIRE(graph.nodes[1].optFormat);
  auto const nativeStorage = std::get<ao::audio::PcmFormat>(*graph.nodes[0].optFormat);
  CHECK(nativeStorage.sampleRate == 48000U);
  CHECK(nativeStorage.channels == 4U);
  CHECK(nativeStorage.encoding == ao::audio::SampleEncoding::Signed32Le);
  CHECK(nativeStorage.speakerMask == 0U);
  CHECK(std::get<ao::audio::SignalFormat>(*graph.nodes[1].optFormat).precisionBits == 24U);
  CHECK(std::get<ao::audio::SignalFormat>(*graph.nodes[1].optFormat).speakerMask == 0U);
  CHECK(graph.nodes[0].channelDelivery == mode.channelDelivery);
  CHECK(graph.nodes[1].channelDelivery == mode.channelDelivery);

  registry.publish({.routeAnchor = "hw:0,0", .optMode = mode});
  CHECK(calls == 2U);
  CHECK(graph.nodes[0].channelDelivery.nativeSpeakerMask == 0U);

  // No format change: current evidence alone must invalidate graph equality.
  mode.channelDelivery.isMapped = true;
  mode.channelDelivery.nativeSpeakerMask = 0x33;
  registry.publish({.routeAnchor = "hw:0,0", .optMode = mode});

  CHECK(calls == 3U);
  CHECK(graph.nodes[0].channelDelivery == mode.channelDelivery);
  CHECK(graph.nodes[1].channelDelivery == mode.channelDelivery);
  CHECK(std::get<ao::audio::PcmFormat>(*graph.nodes[0].optFormat) == nativeStorage);
  CHECK(mode.clientFormat.speakerMask == 0x33U);
}

TEST_CASE("AlsaGraphRegistry - observed endpoint set does not become a canonical native slot description",
          "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto graph = Graph{};
  auto sub = registry.subscribe("hw:0,0", [&](Graph const& nextGraph) { graph = nextGraph; });
  auto const mode = ao::audio::OpenedPcmMode{
    .clientFormat = {.sampleRate = 48000,
                     .channels = 4,
                     .encoding = ao::audio::SampleEncoding::Signed24PackedLe,
                     .speakerMask = 0x33},
    .optEndpoint =
      ao::audio::ConfirmedEndpoint{
        .signalFormat = {.sampleRate = 48000, .channels = 4, .precisionBits = 24, .speakerMask = 0x33}},
    .channelDelivery = {.isMapped = true, .nativeSpeakerMask = 0x33}};

  registry.publish({.routeAnchor = "hw:0,0", .optMode = mode});

  REQUIRE(graph.nodes.size() == 2U);
  REQUIRE(graph.nodes[0].optFormat);
  REQUIRE(graph.nodes[1].optFormat);
  CHECK(std::get<ao::audio::PcmFormat>(*graph.nodes[0].optFormat).speakerMask == 0U);
  CHECK(std::get<ao::audio::SignalFormat>(*graph.nodes[1].optFormat).speakerMask == 0x33U);
  CHECK(graph.nodes[0].channelDelivery.isMapped);
  CHECK(graph.nodes[0].channelDelivery.nativeSpeakerMask == 0x33U);
  CHECK(graph.nodes[1].channelDelivery.nativeSpeakerMask == 0x33U);
  CHECK_FALSE(graph.nodes[0].channelDelivery.hasSharedConversion);
}

TEST_CASE("AlsaGraphRegistry - initial subscription receives neutral graph", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};
  std::int32_t callCount = 0;

  auto sub = registry.subscribe("hw:0,0",
                                [&](Graph const& g)
                                {
                                  receivedGraph = g;
                                  callCount++;
                                });

  CHECK(callCount == 1);
  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK(receivedGraph.nodes[1].id == "alsa-sink");
  CHECK(receivedGraph.nodes[1].name == "hw:0,0");
  CHECK_FALSE(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].softwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].isMuted);
  REQUIRE(receivedGraph.connections.size() == 1);
  CHECK_FALSE(receivedGraph.connections.front().isActive);
}

TEST_CASE("AlsaGraphRegistry - hardware volume publish updates subscribers", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 0.5F, .muted = false, .volumeMode = AlsaVolumeControlMode::HardwareMixer});

  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].softwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].isMuted);
}

TEST_CASE("AlsaGraphRegistry - stream shows the client bytes and sink shows the endpoint signal", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  // A 32-bit container in front of a converter that only resolves 24 bits.
  // Copying one format into both nodes, as the registry used to, would report
  // this endpoint as 32-bit and hide the truncation it performs.
  auto const clientFormat =
    ao::audio::PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = ao::audio::SampleEncoding::Signed32Le};
  auto const endpointSignal = ao::audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 24};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0",
     .optMode = ao::audio::OpenedPcmMode{
       .clientFormat = clientFormat, .optEndpoint = ao::audio::ConfirmedEndpoint{.signalFormat = endpointSignal}}});

  REQUIRE(receivedGraph.nodes.size() == 2);
  REQUIRE(receivedGraph.nodes[0].optFormat);
  REQUIRE(receivedGraph.nodes[1].optFormat);
  CHECK(std::get<ao::audio::PcmFormat>(*receivedGraph.nodes[0].optFormat) == clientFormat);
  CHECK(std::get<ao::audio::SignalFormat>(*receivedGraph.nodes[1].optFormat) == endpointSignal);
  REQUIRE(receivedGraph.connections.size() == 1);
  CHECK(receivedGraph.connections.front().isActive);
}

TEST_CASE("AlsaGraphRegistry - an unconfirmed endpoint leaves the sink format empty", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};
  auto const clientFormat =
    ao::audio::PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = ao::audio::SampleEncoding::Signed24PackedLe};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish({.routeAnchor = "hw:0,0", .optMode = ao::audio::OpenedPcmMode{.clientFormat = clientFormat}});

  REQUIRE(receivedGraph.nodes.size() == 2);
  REQUIRE(receivedGraph.nodes[0].optFormat);
  CHECK(std::get<ao::audio::PcmFormat>(*receivedGraph.nodes[0].optFormat) == clientFormat);
  CHECK_FALSE(receivedGraph.nodes[1].optFormat);
}

TEST_CASE("AlsaGraphRegistry - software volume publish updates subscribers", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 0.8F, .muted = false, .volumeMode = AlsaVolumeControlMode::SoftwareGain});

  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK_FALSE(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK(receivedGraph.nodes[1].softwareVolumeNotUnity);
  CHECK(receivedGraph.nodes[1].maxSoftwareGain == 0.8F);
  CHECK_FALSE(receivedGraph.nodes[1].isMuted);
}

TEST_CASE("AlsaGraphRegistry - software amplification publishes gain magnitude", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 1.25F, .muted = false, .volumeMode = AlsaVolumeControlMode::SoftwareGain});

  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK_FALSE(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK(receivedGraph.nodes[1].softwareVolumeNotUnity);
  CHECK(receivedGraph.nodes[1].maxSoftwareGain == 1.25F);
  CHECK_FALSE(receivedGraph.nodes[1].isMuted);
}

TEST_CASE("AlsaGraphRegistry - mute publish updates subscribers", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 1.0F, .muted = true, .volumeMode = AlsaVolumeControlMode::HardwareMixer});

  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK_FALSE(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK(receivedGraph.nodes[1].isMuted);
}

TEST_CASE("AlsaGraphRegistry - unavailable mode with non-unity volume emits unclassified volume", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 0.5F, .muted = false, .volumeMode = AlsaVolumeControlMode::Unavailable});

  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK_FALSE(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].softwareVolumeNotUnity);
  CHECK(receivedGraph.nodes[1].unclassifiedVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].isMuted);

  // Unavailable mode can retain a cached requested volume while no PCM route is active, so the
  // value is volume intent rather than evidence of an applied gain. It must not reach the user as
  // a numeric magnitude.
  REQUIRE(receivedGraph.connections.size() == 1);
  CHECK_FALSE(receivedGraph.connections[0].isActive);
  CHECK(receivedGraph.nodes[1].maxUnclassifiedGain == 0.0F);
  CHECK(receivedGraph.nodes[1].minUnclassifiedGain == 0.0F);
}

TEST_CASE("AlsaGraphRegistry - clear emits empty graph", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 0.5F, .muted = false, .volumeMode = AlsaVolumeControlMode::HardwareMixer});

  REQUIRE_FALSE(receivedGraph.nodes.empty());

  registry.clear("hw:0,0");
  CHECK(receivedGraph.nodes.empty());
  CHECK(receivedGraph.connections.empty());
}

TEST_CASE("AlsaGraphRegistry - subscriber only receives updates for its anchor", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  std::int32_t callCountA = 0;
  std::int32_t callCountB = 0;

  auto subA = registry.subscribe("hw:0,0", [&](Graph const&) { callCountA++; });
  auto subB = registry.subscribe("hw:1,0", [&](Graph const&) { callCountB++; });

  // Initial snapshots
  CHECK(callCountA == 1);
  CHECK(callCountB == 1);

  registry.publish({.routeAnchor = "hw:0,0", .volume = 0.5F});
  CHECK(callCountA == 2);
  CHECK(callCountB == 1);

  registry.publish({.routeAnchor = "hw:1,0", .volume = 0.5F});
  CHECK(callCountA == 2);
  CHECK(callCountB == 2);
}

TEST_CASE("AlsaGraphRegistry - subscription reset stops updates", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  std::int32_t callCount = 0;

  auto sub = registry.subscribe("hw:0,0", [&](Graph const&) { callCount++; });
  CHECK(callCount == 1);

  sub.reset();

  registry.publish({.routeAnchor = "hw:0,0", .volume = 0.5F});
  CHECK(callCount == 1);
}

TEST_CASE("AlsaGraphRegistry - volume callback may publish reentrantly", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  std::int32_t callbackCount = 0;
  std::int32_t callbackDepth = 0;
  std::int32_t maximumDepth = 0;
  bool nestedPublish = false;
  float observedVolume = 1.0F;
  auto sub = registry.subscribe(
    "hw:0,0",
    [&](Graph const& graph)
    {
      ++callbackDepth;
      maximumDepth = std::max(maximumDepth, callbackDepth);
      ++callbackCount;

      if (graph.nodes.size() == 2)
      {
        observedVolume = graph.nodes[1].maxSoftwareGain;
      }

      if (callbackCount == 2 && !nestedPublish)
      {
        nestedPublish = true;
        registry.publish({.routeAnchor = "hw:0,0", .volume = 0.25F, .volumeMode = AlsaVolumeControlMode::SoftwareGain});
      }

      --callbackDepth;
    });

  registry.publish({.routeAnchor = "hw:0,0", .volume = 0.5F, .volumeMode = AlsaVolumeControlMode::SoftwareGain});

  CHECK(callbackCount == 3);
  CHECK(maximumDepth == 1);
  CHECK(observedVolume == 0.25F);
}

TEST_CASE("AlsaGraphRegistry - cancellation removes a callback already copied for publication",
          "[audio][unit][alsa][subscription][concurrency]")
{
  auto registry = AlsaGraphRegistry{};
  bool cancelSecond = false;
  std::int32_t firstCalls = 0;
  std::int32_t secondCalls = 0;
  auto secondSub = ao::utility::ScopedRegistration{};
  auto firstSub = registry.subscribe("hw:0,0",
                                     [&](Graph const&)
                                     {
                                       ++firstCalls;

                                       if (cancelSecond)
                                       {
                                         secondSub.reset();
                                       }
                                     });
  secondSub = registry.subscribe("hw:0,0", [&](Graph const&) { ++secondCalls; });
  cancelSecond = true;

  registry.publish({.routeAnchor = "hw:0,0", .volume = 0.5F});

  CHECK(firstCalls == 2);
  CHECK(secondCalls == 1);
}

TEST_CASE("AlsaGraphRegistry - shutdown retires subscriptions and retained publishers",
          "[audio][unit][alsa][concurrency]")
{
  auto registry = AlsaGraphRegistry{};
  auto publisher = registry.publisher();
  auto graphs = std::vector<Graph>{};
  auto sub = registry.subscribe("hw:0,0", [&](Graph const& graph) { graphs.push_back(graph); });
  REQUIRE(sub);

  publisher.publish({.routeAnchor = "hw:0,0", .volume = 0.5F});
  registry.shutdown();
  registry.shutdown();
  publisher.publish({.routeAnchor = "hw:0,0", .volume = 0.25F});
  publisher.clear("hw:0,0");

  REQUIRE(graphs.size() == 3U);
  CHECK(graphs.back().nodes.empty());
  CHECK(graphs.back().connections.empty());
  std::int32_t lateCalls = 0;
  CHECK_FALSE(registry.subscribe("hw:0,0", [&](Graph const&) { ++lateCalls; }));
  CHECK(lateCalls == 0);
  sub.reset();
}

TEST_CASE("AlsaGraphRegistry - publisher and subscription may outlive registry", "[audio][unit][alsa][concurrency]")
{
  auto publisher = AlsaGraphPublisher{};
  auto sub = ao::utility::ScopedRegistration{};
  std::int32_t callbackCount = 0;

  {
    auto registryPtr = std::make_unique<AlsaGraphRegistry>();
    publisher = registryPtr->publisher();
    sub = registryPtr->subscribe("hw:0,0", [&](Graph const&) { ++callbackCount; });
    REQUIRE(sub);
  }

  CHECK(callbackCount == 2);
  publisher.publish({.routeAnchor = "hw:0,0", .volume = 0.5F});
  publisher.clear("hw:0,0");
  CHECK(callbackCount == 2);
  sub.reset();
  CHECK_FALSE(sub);
}
