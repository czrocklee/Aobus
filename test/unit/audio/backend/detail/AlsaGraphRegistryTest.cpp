// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaGraphRegistry.h"

#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

using namespace ao::audio::backend::detail;
using namespace ao::audio::flow;

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

TEST_CASE("AlsaGraphRegistry - initial callback defers registry teardown", "[audio][regression][alsa][concurrency]")
{
  auto registryPtr = std::make_unique<AlsaGraphRegistry>();
  std::int32_t callbackCount = 0;
  bool teardownRequested = false;

  auto sub = registryPtr->subscribe("hw:0,0",
                                    [&](Graph const&)
                                    {
                                      ++callbackCount;
                                      teardownRequested = true;
                                    });

  CHECK(callbackCount == 1);
  CHECK(teardownRequested);
  REQUIRE(registryPtr);
  sub.reset();
  registryPtr.reset();
  CHECK_FALSE(registryPtr);
}

TEST_CASE("AlsaGraphRegistry - publication callback defers registry teardown", "[audio][regression][alsa][concurrency]")
{
  auto registryPtr = std::make_unique<AlsaGraphRegistry>();
  std::int32_t callbackCount = 0;
  bool teardownRequested = false;
  auto sub = registryPtr->subscribe("hw:0,0",
                                    [&](Graph const&)
                                    {
                                      ++callbackCount;

                                      if (callbackCount == 2)
                                      {
                                        teardownRequested = true;
                                      }
                                    });

  registryPtr->publish({.routeAnchor = "hw:0,0", .volume = 0.5F, .volumeMode = AlsaVolumeControlMode::HardwareMixer});

  CHECK(callbackCount == 2);
  CHECK(teardownRequested);
  REQUIRE(registryPtr);
  sub.reset();
  registryPtr.reset();
  CHECK_FALSE(registryPtr);
}

TEST_CASE("AlsaGraphRegistry - volume callback may publish reentrantly", "[audio][regression][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  std::int32_t callbackCount = 0;
  bool nestedPublish = false;
  float observedVolume = 1.0F;
  auto sub = registry.subscribe(
    "hw:0,0",
    [&](Graph const& graph)
    {
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
    });

  registry.publish({.routeAnchor = "hw:0,0", .volume = 0.5F, .volumeMode = AlsaVolumeControlMode::SoftwareGain});

  CHECK(callbackCount == 3);
  CHECK(observedVolume == 0.25F);
}

TEST_CASE("AlsaGraphRegistry - cancellation removes a callback already copied for publication",
          "[audio][regression][alsa][subscription]")
{
  auto registry = AlsaGraphRegistry{};
  bool cancelSecond = false;
  std::int32_t firstCalls = 0;
  std::int32_t secondCalls = 0;
  auto secondSub = ao::audio::Subscription{};
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
