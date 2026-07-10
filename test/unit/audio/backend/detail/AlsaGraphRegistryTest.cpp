// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Format.h>
#include <ao/audio/backend/detail/AlsaGraphRegistry.h>
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

TEST_CASE("AlsaGraphRegistry - published negotiated format reaches stream and sink", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};
  auto const negotiatedFormat = ao::audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 16};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish({.routeAnchor = "hw:0,0", .optFormat = negotiatedFormat});

  REQUIRE(receivedGraph.nodes.size() == 2);
  REQUIRE(receivedGraph.nodes[0].optFormat);
  REQUIRE(receivedGraph.nodes[1].optFormat);
  CHECK(*receivedGraph.nodes[0].optFormat == negotiatedFormat);
  CHECK(*receivedGraph.nodes[1].optFormat == negotiatedFormat);
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

TEST_CASE("AlsaGraphRegistry - initial callback may destroy registry", "[audio][regression][alsa][lifecycle]")
{
  auto registryPtr = std::make_unique<AlsaGraphRegistry>();
  std::int32_t callbackCount = 0;

  auto sub = registryPtr->subscribe("hw:0,0",
                                    [&](Graph const&)
                                    {
                                      ++callbackCount;
                                      registryPtr.reset();
                                    });

  CHECK(callbackCount == 1);
  CHECK_FALSE(registryPtr);
  CHECK_FALSE(sub);
  sub.reset();
}

TEST_CASE("AlsaGraphRegistry - subscription may outlive registry", "[audio][regression][alsa][lifecycle]")
{
  auto sub = ao::audio::Subscription{};

  {
    auto registry = AlsaGraphRegistry{};
    sub = registry.subscribe("hw:0,0", [](Graph const&) {});
  }

  sub.reset();
  CHECK_FALSE(sub);
}

TEST_CASE("AlsaGraphRegistry - volume callback may destroy registry", "[audio][regression][alsa][lifecycle]")
{
  auto registryPtr = std::make_unique<AlsaGraphRegistry>();
  std::int32_t callbackCount = 0;
  auto sub = registryPtr->subscribe("hw:0,0",
                                    [&](Graph const&)
                                    {
                                      ++callbackCount;

                                      if (callbackCount == 2)
                                      {
                                        registryPtr.reset();
                                      }
                                    });

  registryPtr->publish({.routeAnchor = "hw:0,0", .volume = 0.5F, .volumeMode = AlsaVolumeControlMode::HardwareMixer});

  CHECK(callbackCount == 2);
  CHECK_FALSE(registryPtr);
  sub.reset();
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
